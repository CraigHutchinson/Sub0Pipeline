// src/remote/stream_strategy.cpp
//
// The "stream" payload strategy: payload bytes in chunked payload frames on
// the connection's own transport.

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#include "sub0pipeline/remote/strategy.hpp"

namespace sub0pipeline::remote
{

namespace
{

// Chunk body: u8 kind | u64 stream | u64 total | u64 offset | bytes.
constexpr uint8_t     kChunk       = 1U;
constexpr std::size_t kChunkHeader = 1U + 8U + 8U + 8U;

void putU64(std::byte* out, uint64_t v) noexcept
{
    for (std::size_t i = 0U; i < 8U; ++i) out[i] = static_cast<std::byte>((v >> (8U * i)) & 0xFFU);
}

uint64_t getU64(const std::byte* in) noexcept
{
    uint64_t v = 0U;
    for (std::size_t i = 0U; i < 8U; ++i) v |= static_cast<uint64_t>(std::to_integer<uint8_t>(in[i])) << (8U * i);
    return v;
}

std::vector<std::byte> encodeU32(uint32_t v)
{
    std::vector<std::byte> out(4U);
    for (std::size_t i = 0U; i < 4U; ++i) out[i] = static_cast<std::byte>((v >> (8U * i)) & 0xFFU);
    return out;
}

std::optional<uint32_t> decodeU32(std::span<const std::byte> in) noexcept
{
    if (in.size() != 4U) return std::nullopt;
    uint32_t v = 0U;
    for (std::size_t i = 0U; i < 4U; ++i) v |= static_cast<uint32_t>(std::to_integer<uint8_t>(in[i])) << (8U * i);
    return v;
}

class StreamStrategy final : public IPayloadStrategy
{
public:
    explicit StreamStrategy(StreamOptions options) noexcept
        : chunkBytes_{std::clamp<uint32_t>(options.chunkBytes, 1U, kMaxChunk)} {}

    std::string_view name() const noexcept override { return "stream"; }

    std::vector<std::byte> offer() override { return encodeU32(chunkBytes_); }

    std::expected<std::vector<std::byte>, PipelineError> accept(std::span<const std::byte> offer) override
    {
        const auto peer = decodeU32(offer);
        if (!peer || *peer == 0U) return std::unexpected(PipelineError::kJobFailed);
        chunkBytes_ = std::min(chunkBytes_, *peer);
        return encodeU32(chunkBytes_);
    }

    std::expected<void, PipelineError> complete(std::span<const std::byte> accepted) override
    {
        const auto peer = decodeU32(accepted);
        if (!peer || *peer == 0U) return std::unexpected(PipelineError::kJobFailed);
        chunkBytes_ = std::min(chunkBytes_, *peer);
        return {};
    }

    void attach(IPayloadSender& sender) override { sender_ = &sender; }

    std::expected<std::vector<std::byte>, PipelineError> send(const Payload& payload, uint64_t) override
    {
        const auto bytes = payload.bytes();
        const uint64_t stream = nextStream_.fetch_add(1U, std::memory_order_relaxed);
        std::size_t offset = 0U;
        do
        {
            const auto length = std::min<std::size_t>(chunkBytes_, bytes.size() - offset);
            std::byte header[kChunkHeader];
            header[0] = static_cast<std::byte>(kChunk);
            putU64(header + 1, stream);
            putU64(header + 9, bytes.size());
            putU64(header + 17, offset);
            const std::span<const std::byte> parts[] = {header, bytes.subspan(offset, length)};
            if (!sender_ || !sender_->sendPayloadFrame(parts)) return std::unexpected(PipelineError::kUnavailable);
            offset += length;
        } while (offset < bytes.size());
        std::vector<std::byte> reference(8U);
        putU64(reference.data(), stream);
        return reference;
    }

    std::expected<Payload, PipelineError> receive(std::span<const std::byte> reference) override
    {
        if (reference.size() != 8U) return std::unexpected(PipelineError::kJobFailed);
        std::lock_guard lock{mtx_};
        auto found = complete_.find(getU64(reference.data()));
        if (found == complete_.end()) return std::unexpected(PipelineError::kJobFailed);
        auto payload = std::move(found->second);
        complete_.erase(found);
        return payload;
    }

    void onPayloadFrame(const FrameView& frame) override
    {
        if (frame.body.size() < kChunkHeader || std::to_integer<uint8_t>(frame.body[0]) != kChunk) return;
        const auto stream = getU64(frame.body.data() + 1);
        const auto total = getU64(frame.body.data() + 9);
        const auto offset = getU64(frame.body.data() + 17);
        const auto chunk = frame.body.subspan(kChunkHeader);
        if (offset > total || chunk.size() > total - offset) return;

        std::lock_guard lock{mtx_};
        if (offset == 0U && chunk.size() == total && frame.owner)
        {
            // The whole payload is in this frame: keep its buffer, copy nothing.
            complete_[stream] = Payload::view(chunk, frame.owner);
            return;
        }
        auto& partial = partial_[stream];
        if (!partial.storage)
        {
            partial.storage.reset(new std::byte[total == 0U ? 1U : total]);
            partial.total = total;
        }
        if (partial.total != total) return;
        if (!chunk.empty()) std::memcpy(partial.storage.get() + offset, chunk.data(), chunk.size());
        partial.received += chunk.size();
        if (partial.received >= partial.total)
        {
            auto storage = std::move(partial.storage);
            const std::span<const std::byte> bytes{storage.get(), static_cast<std::size_t>(total)};
            complete_[stream] = Payload::view(bytes, std::shared_ptr<const void>{storage, storage.get()});
            partial_.erase(stream);
        }
    }

    void onClosed() noexcept override
    {
        std::lock_guard lock{mtx_};
        partial_.clear();
        complete_.clear();
    }

private:
    // Leaves room for the chunk header within a u32 frame body.
    static constexpr uint32_t kMaxChunk = UINT32_MAX - 64U;

    struct Partial
    {
        std::shared_ptr<std::byte[]> storage;
        uint64_t                     total{0U};
        uint64_t                     received{0U};
    };

    uint32_t                               chunkBytes_;
    IPayloadSender*                        sender_{nullptr};
    std::atomic<uint64_t>                  nextStream_{1U};
    std::mutex                             mtx_;
    std::unordered_map<uint64_t, Partial>  partial_;
    std::unordered_map<uint64_t, Payload>  complete_;
};

} // namespace

PayloadStrategyFactory streamStrategy(StreamOptions options)
{
    return [options] { return std::make_unique<StreamStrategy>(options); };
}

} // namespace sub0pipeline::remote
