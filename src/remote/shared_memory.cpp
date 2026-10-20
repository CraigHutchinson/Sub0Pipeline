// src/remote/shared_memory.cpp
//
// The "shm" payload strategy: each side allocates payloads in its own mapped
// region and the peer reads them in place.

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "shared_region.hpp"
#include "sub0pipeline/remote/shared_memory.hpp"

namespace sub0pipeline::remote
{

namespace
{

constexpr std::size_t kAlignment = 64U;   // a cache line, so neighbouring payloads never share one
constexpr uint8_t     kInRegion  = 0U;    // reference: u8 | u64 block | u64 offset | u64 size
constexpr uint8_t     kStreamed  = 1U;    // reference: u8 | the fallback stream's reference
constexpr uint8_t     kRelease   = 0x52U; // frame: u8 | u64 block; distinct from stream chunk frames

void putU64(std::vector<std::byte>& out, uint64_t v)
{
    for (std::size_t i = 0U; i < 8U; ++i) out.push_back(static_cast<std::byte>((v >> (8U * i)) & 0xFFU));
}

uint64_t getU64(const std::byte* in) noexcept
{
    uint64_t v = 0U;
    for (std::size_t i = 0U; i < 8U; ++i) v |= static_cast<uint64_t>(std::to_integer<uint8_t>(in[i])) << (8U * i);
    return v;
}

// Setup: u64 region size | u16 name length | name | the fallback stream's setup.
std::vector<std::byte> encodeSetup(std::size_t size, const std::string& name, std::span<const std::byte> stream)
{
    std::vector<std::byte> out;
    putU64(out, size);
    const auto length = static_cast<uint16_t>(std::min<std::size_t>(name.size(), UINT16_MAX));
    out.push_back(static_cast<std::byte>(length & 0xFFU));
    out.push_back(static_cast<std::byte>(length >> 8U));
    for (std::size_t i = 0U; i < length; ++i) out.push_back(static_cast<std::byte>(name[i]));
    out.insert(out.end(), stream.begin(), stream.end());
    return out;
}

struct Setup
{
    std::size_t                size{0U};
    std::string                name;
    std::span<const std::byte> stream;
};

std::optional<Setup> decodeSetup(std::span<const std::byte> in)
{
    if (in.size() < 10U) return std::nullopt;
    Setup setup;
    setup.size = static_cast<std::size_t>(getU64(in.data()));
    const auto length = static_cast<std::size_t>(std::to_integer<uint8_t>(in[8]))
                      | (static_cast<std::size_t>(std::to_integer<uint8_t>(in[9])) << 8U);
    if (in.size() < 10U + length || length == 0U) return std::nullopt;
    for (std::size_t i = 0U; i < length; ++i) setup.name.push_back(static_cast<char>(in[10U + i]));
    setup.stream = in.subspan(10U + length);
    return setup;
}

} // namespace

struct SharedMemoryStrategy::State
{
    enum class Role : uint8_t { kUnknown, kCoordinator, kWorker };

    struct Block
    {
        std::size_t offset{0U};
        std::size_t size{0U};      ///< Allocated (aligned) size.
        std::byte*  data{nullptr}; ///< Start in this process's mapping.
        uint32_t    localRefs{0U}; ///< Live payloads on this side that view the block.
        uint32_t    peerHolds{0U}; ///< Sends the peer has not finished with.
    };

    explicit State(SharedMemoryOptions o) : options{o} {}

    SharedMemoryOptions                                  options;
    Role                                                 role{Role::kUnknown};
    std::shared_ptr<SharedRegion>                        own;   ///< Dropped with the strategy; in-place leases hold their own reference.
    std::shared_ptr<SharedRegion>                        peer;

    // Held while a release is being sent, so onClosed() can retire the sender.
    std::mutex                                           senderMtx;
    IPayloadSender*                                      sender{nullptr};
    bool                                                 closed{false};

    mutable std::mutex                                   mtx;
    std::map<std::size_t, std::size_t>                   free;      ///< offset -> size, coalesced
    std::size_t                                          inUse{0U};
    std::unordered_map<uint64_t, Block>                  blocks;
    std::unordered_map<const std::byte*, uint64_t>       byAddress; ///< In-place buffers, for send() to recognise
    std::unordered_map<uint64_t, std::vector<uint64_t>>  scopes;    ///< Coordinator: request -> its input blocks
    uint64_t                                             nextBlock{1U};

    void initFree()
    {
        free.clear();
        if (own && own->size() >= kAlignment) free.emplace(0U, own->size() - own->size() % kAlignment);
    }

    // With mtx held. First fit; returns the block id, or 0 when nothing fits.
    uint64_t allocateBlock(std::size_t bytes)
    {
        if (!own) return 0U;
        const auto size = std::max(kAlignment, (bytes + kAlignment - 1U) / kAlignment * kAlignment);
        for (auto it = free.begin(); it != free.end(); ++it)
        {
            if (it->second < size) continue;
            const auto offset = it->first;
            const auto remaining = it->second - size;
            free.erase(it);
            if (remaining != 0U) free.emplace(offset + size, remaining);
            inUse += size;
            const auto id = nextBlock++;
            blocks.emplace(id, Block{offset, size, own->data() + offset, 0U, 0U});
            return id;
        }
        return 0U;
    }

    // With mtx held.
    void releaseIfUnused(uint64_t id)
    {
        auto found = blocks.find(id);
        if (found == blocks.end() || found->second.localRefs != 0U || found->second.peerHolds != 0U) return;
        const auto block = found->second;
        blocks.erase(found);
        byAddress.erase(block.data);
        inUse -= block.size;
        auto [it, inserted] = free.emplace(block.offset, block.size);
        if (!inserted) return;
        if (auto next = std::next(it); next != free.end() && it->first + it->second == next->first)
        {
            it->second += next->second;
            free.erase(next);
        }
        if (it != free.begin())
        {
            auto previous = std::prev(it);
            if (previous->first + previous->second == it->first)
            {
                previous->second += it->second;
                free.erase(it);
            }
        }
    }

    void dropLocal(uint64_t id)
    {
        std::lock_guard lock{mtx};
        if (auto found = blocks.find(id); found != blocks.end() && found->second.localRefs != 0U)
        {
            --found->second.localRefs;
            releaseIfUnused(id);
        }
    }

    void dropPeerHold(uint64_t id) // with mtx held
    {
        if (auto found = blocks.find(id); found != blocks.end() && found->second.peerHolds != 0U)
        {
            --found->second.peerHolds;
            releaseIfUnused(id);
        }
    }

    void sendRelease(uint64_t id) noexcept
    {
        std::lock_guard lock{senderMtx};
        if (closed || !sender) return;
        std::byte body[9];
        body[0] = static_cast<std::byte>(kRelease);
        for (std::size_t i = 0U; i < 8U; ++i) body[1U + i] = static_cast<std::byte>((id >> (8U * i)) & 0xFFU);
        const std::span<const std::byte> parts[] = {body};
        (void)sender->sendPayloadFrame(parts);
    }

    // Keeps an in-place buffer allocated while any payload views it.
    struct LocalLease
    {
        std::shared_ptr<State>        state;
        std::shared_ptr<SharedRegion> region; ///< Keeps the bytes mapped after the strategy is gone.
        uint64_t                      id;
        LocalLease(std::shared_ptr<State> s, std::shared_ptr<SharedRegion> r, uint64_t i) noexcept
            : state{std::move(s)}, region{std::move(r)}, id{i} {}
        LocalLease(const LocalLease&) = delete;
        LocalLease& operator=(const LocalLease&) = delete;
        ~LocalLease() { state->dropLocal(id); }
    };

    // Keeps the peer's region mapped while a payload views it; on the
    // coordinator, also tells the worker when the last view is gone.
    struct PeerLease
    {
        std::shared_ptr<State>        state;
        std::shared_ptr<SharedRegion> region;
        uint64_t                      id;
        bool                          release;
        PeerLease(std::shared_ptr<State> s, std::shared_ptr<SharedRegion> r, uint64_t i, bool rel) noexcept
            : state{std::move(s)}, region{std::move(r)}, id{i}, release{rel} {}
        PeerLease(const PeerLease&) = delete;
        PeerLease& operator=(const PeerLease&) = delete;
        ~PeerLease()
        {
            if (release) state->sendRelease(id);
        }
    };
};

SharedMemoryStrategy::SharedMemoryStrategy(SharedMemoryOptions options)
    : state_{std::make_shared<State>(options)}, fallback_{streamStrategy(options.fallback)()} {}

SharedMemoryStrategy::~SharedMemoryStrategy()
{
    // Leases may keep the state alive; they must not keep this side's region
    // mapped (or, on Windows, named) beyond the buffers they view.
    std::lock_guard lock{state_->mtx};
    state_->own.reset();
}

std::vector<std::byte> SharedMemoryStrategy::offer()
{
    std::lock_guard lock{state_->mtx};
    state_->role = State::Role::kCoordinator;
    state_->own = SharedRegion::create(state_->options.regionBytes);
    if (!state_->own) return {}; // an empty offer makes the worker refuse, and negotiation move on
    state_->initFree();
    return encodeSetup(state_->own->size(), state_->own->name(), fallback_->offer());
}

std::expected<std::vector<std::byte>, PipelineError> SharedMemoryStrategy::accept(std::span<const std::byte> offer)
{
    const auto setup = decodeSetup(offer);
    if (!setup) return std::unexpected(PipelineError::kUnavailable);
    std::shared_ptr<SharedRegion> peer = SharedRegion::open(setup->name, setup->size);
    if (!peer) return std::unexpected(PipelineError::kUnavailable); // not the same machine or namespace
    auto stream = fallback_->accept(setup->stream);
    if (!stream) return std::unexpected(stream.error());
    std::lock_guard lock{state_->mtx};
    state_->role = State::Role::kWorker;
    state_->peer = std::move(peer);
    state_->own = SharedRegion::create(state_->options.regionBytes);
    if (!state_->own) return std::unexpected(PipelineError::kUnavailable);
    state_->initFree();
    return encodeSetup(state_->own->size(), state_->own->name(), *stream);
}

std::expected<void, PipelineError> SharedMemoryStrategy::complete(std::span<const std::byte> accepted)
{
    const auto setup = decodeSetup(accepted);
    if (!setup) return std::unexpected(PipelineError::kUnavailable);
    std::shared_ptr<SharedRegion> peer = SharedRegion::open(setup->name, setup->size);
    if (!peer) return std::unexpected(PipelineError::kUnavailable);
    if (auto stream = fallback_->complete(setup->stream); !stream) return stream;
    std::lock_guard lock{state_->mtx};
    state_->peer = std::move(peer);
    // Both sides have mapped both regions: from now on they live only while mapped.
    if (state_->own) state_->own->unlink();
    state_->peer->unlink();
    return {};
}

void SharedMemoryStrategy::attach(IPayloadSender& sender)
{
    {
        std::lock_guard lock{state_->senderMtx};
        state_->sender = &sender;
    }
    fallback_->attach(sender);
}

std::expected<std::vector<std::byte>, PipelineError> SharedMemoryStrategy::send(const Payload& payload, uint64_t scope)
{
    const auto bytes = payload.bytes();
    std::unique_lock lock{state_->mtx};
    uint64_t id = 0U;
    bool copy = false;
    if (auto found = state_->byAddress.find(bytes.data()); found != state_->byAddress.end())
    {
        id = found->second; // an in-place buffer: no copy
    }
    else
    {
        id = state_->allocateBlock(bytes.size());
        copy = true;
    }
    if (id == 0U)
    {
        lock.unlock();
        auto streamed = fallback_->send(payload, scope);
        if (!streamed) return streamed;
        std::vector<std::byte> reference{static_cast<std::byte>(kStreamed)};
        reference.insert(reference.end(), streamed->begin(), streamed->end());
        return reference;
    }
    auto& block = state_->blocks.at(id);
    ++block.peerHolds; // keeps the block while the copy below runs unlocked
    if (state_->role == State::Role::kCoordinator) state_->scopes[scope].push_back(id);
    std::byte* target = block.data;
    const auto offset = block.offset;
    lock.unlock();

    if (copy && !bytes.empty()) std::memcpy(target, bytes.data(), bytes.size()); // the copy this medium needs
    std::vector<std::byte> reference{static_cast<std::byte>(kInRegion)};
    putU64(reference, id);
    putU64(reference, offset);
    putU64(reference, bytes.size());
    return reference;
}

std::expected<Payload, PipelineError> SharedMemoryStrategy::receive(std::span<const std::byte> reference)
{
    if (reference.empty()) return std::unexpected(PipelineError::kJobFailed);
    if (std::to_integer<uint8_t>(reference[0]) == kStreamed) return fallback_->receive(reference.subspan(1U));
    if (std::to_integer<uint8_t>(reference[0]) != kInRegion || reference.size() != 25U)
        return std::unexpected(PipelineError::kJobFailed);
    const auto id = getU64(reference.data() + 1);
    const auto offset = getU64(reference.data() + 9);
    const auto size = getU64(reference.data() + 17);
    std::shared_ptr<SharedRegion> peer;
    bool release;
    {
        std::lock_guard lock{state_->mtx};
        peer = state_->peer;
        release = state_->role == State::Role::kCoordinator;
    }
    if (!peer || offset > peer->size() || size > peer->size() - offset) return std::unexpected(PipelineError::kJobFailed);
    auto lease = std::make_shared<State::PeerLease>(state_, peer, id, release);
    const std::span<const std::byte> bytes{peer->data() + offset, static_cast<std::size_t>(size)};
    return Payload::view(bytes, std::shared_ptr<const void>{std::move(lease)});
}

void SharedMemoryStrategy::onPayloadFrame(const FrameView& frame)
{
    if (frame.body.size() == 9U && std::to_integer<uint8_t>(frame.body[0]) == kRelease)
    {
        std::lock_guard lock{state_->mtx};
        state_->dropPeerHold(getU64(frame.body.data() + 1));
        return;
    }
    fallback_->onPayloadFrame(frame);
}

void SharedMemoryStrategy::endScope(uint64_t scope) noexcept
{
    std::lock_guard lock{state_->mtx};
    auto found = state_->scopes.find(scope);
    if (found == state_->scopes.end()) return;
    for (auto id : found->second) state_->dropPeerHold(id);
    state_->scopes.erase(found);
    fallback_->endScope(scope);
}

std::optional<WritablePayload> SharedMemoryStrategy::allocate(std::size_t bytes)
{
    if (bytes == 0U) return std::nullopt;
    std::lock_guard lock{state_->mtx};
    if (!state_->own) return std::nullopt;
    const auto id = state_->allocateBlock(bytes);
    if (id == 0U) return std::nullopt;
    auto& block = state_->blocks.at(id);
    block.localRefs = 1U;
    std::byte* data = block.data;
    state_->byAddress[data] = id;
    auto lease = std::make_shared<State::LocalLease>(state_, state_->own, id);
    const std::span<std::byte> writable{data, bytes};
    return WritablePayload{Payload::view(writable, std::shared_ptr<const void>{std::move(lease)}), writable};
}

void SharedMemoryStrategy::onClosed() noexcept
{
    {
        std::lock_guard lock{state_->senderMtx};
        state_->closed = true;
        state_->sender = nullptr;
    }
    {
        // The peer is gone: nothing it held will be released by it.
        std::lock_guard lock{state_->mtx};
        state_->scopes.clear();
        std::vector<uint64_t> ids;
        ids.reserve(state_->blocks.size());
        for (auto& [id, block] : state_->blocks)
        {
            block.peerHolds = 0U;
            ids.push_back(id);
        }
        for (auto id : ids) state_->releaseIfUnused(id);
    }
    fallback_->onClosed();
}

std::size_t SharedMemoryStrategy::bytesInUse() const
{
    std::lock_guard lock{state_->mtx};
    return state_->inUse;
}

std::string SharedMemoryStrategy::regionName() const
{
    std::lock_guard lock{state_->mtx};
    return state_->own ? state_->own->name() : std::string{};
}

PayloadStrategyFactory sharedMemoryStrategy(SharedMemoryOptions options)
{
    return [options] { return std::make_unique<SharedMemoryStrategy>(options); };
}

} // namespace sub0pipeline::remote
