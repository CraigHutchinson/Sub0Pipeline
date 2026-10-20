// src/remote/store_strategy.cpp
//
// The "store" payload strategy: payload bytes go through the consumer's
// IPayloadStore and only the handle crosses the transport.

#include <memory>
#include <utility>
#include <vector>

#include "sub0pipeline/remote/strategy.hpp"

namespace sub0pipeline::remote
{

namespace
{

class StoreStrategy final : public IPayloadStrategy
{
public:
    explicit StoreStrategy(IPayloadStore& store) noexcept : store_{store} {}

    std::string_view name() const noexcept override { return "store"; }
    std::vector<std::byte> offer() override { return {}; }
    std::expected<std::vector<std::byte>, PipelineError> accept(std::span<const std::byte>) override
    {
        return std::vector<std::byte>{};
    }
    std::expected<void, PipelineError> complete(std::span<const std::byte>) override { return {}; }
    void attach(IPayloadSender&) override {}

    std::expected<std::vector<std::byte>, PipelineError> send(const Payload& payload, uint64_t) override
    {
        auto handle = store_.put(payload.bytes());
        if (handle && handle->size() > kMaxHandleBytes) return std::unexpected(PipelineError::kJobFailed);
        return handle;
    }

    std::expected<Payload, PipelineError> receive(std::span<const std::byte> reference) override
    {
        auto bytes = store_.get(reference);
        if (!bytes) return std::unexpected(bytes.error());
        return Payload::inlineBytes(std::move(*bytes));
    }

    void onPayloadFrame(const FrameView&) override {}

private:
    IPayloadStore& store_;
};

} // namespace

PayloadStrategyFactory storeStrategy(IPayloadStore& store)
{
    return [&store] { return std::make_unique<StoreStrategy>(store); };
}

} // namespace sub0pipeline::remote
