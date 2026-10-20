// src/remote/registry.cpp
//
// JobRegistry and the registry fingerprint.

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "sub0pipeline/remote/registry.hpp"
#include "sub0pipeline/remote/strategy.hpp"

namespace sub0pipeline::remote
{

namespace
{
constexpr uint64_t kFnvOffset = 0xCBF29CE484222325ULL;
constexpr uint64_t kFnvPrime  = 0x00000100000001B3ULL;

void mix(uint64_t& hash, uint8_t byte) noexcept
{
    hash ^= byte;
    hash *= kFnvPrime;
}
} // namespace

uint64_t registryFingerprint(std::span<const JobKindId> kinds)
{
    std::vector<const JobKindId*> sorted;
    sorted.reserve(kinds.size());
    for (const auto& kind : kinds) sorted.push_back(&kind);
    std::sort(sorted.begin(), sorted.end(), [](const JobKindId* a, const JobKindId* b) { return *a < *b; });

    uint64_t hash = kFnvOffset;
    for (const auto* kind : sorted)
    {
        // Same encoding as a kind on the wire: u16 length, name bytes, u32 version.
        const auto length = static_cast<uint16_t>(std::min<std::size_t>(kind->name.size(), UINT16_MAX));
        mix(hash, static_cast<uint8_t>(length & 0xFFU));
        mix(hash, static_cast<uint8_t>(length >> 8U));
        for (std::size_t i = 0U; i < length; ++i) mix(hash, static_cast<uint8_t>(kind->name[i]));
        for (uint32_t shift = 0U; shift < 32U; shift += 8U)
            mix(hash, static_cast<uint8_t>((kind->version >> shift) & 0xFFU));
    }
    return hash;
}

bool JobRegistry::add(JobKindId kind, JobKindFn fn)
{
    if (!fn) return false;
    return kinds_.try_emplace(std::move(kind), std::move(fn)).second;
}

const JobKindFn* JobRegistry::find(std::string_view name, uint32_t version) const noexcept
{
    // Linear in the number of kinds, without building a key string per lookup.
    for (const auto& [kind, fn] : kinds_)
    {
        if (kind.version == version && kind.name == name) return &fn;
    }
    return nullptr;
}

std::vector<JobKindId> JobRegistry::kinds() const
{
    std::vector<JobKindId> out;
    out.reserve(kinds_.size());
    for (const auto& entry : kinds_) out.push_back(entry.first);
    return out;
}

uint64_t JobRegistry::fingerprint() const
{
    const auto all = kinds();
    return registryFingerprint(all);
}

} // namespace sub0pipeline::remote

namespace sub0pipeline::remote
{

std::span<std::byte> JobContext::allocateOutput(std::size_t size)
{
    if (strategy_)
    {
        if (auto writable = strategy_->allocate(size))
        {
            output_ = std::move(writable->payload);
            return writable->bytes;
        }
    }
    auto writable = makeWritablePayload(size);
    output_ = std::move(writable.payload);
    return writable.bytes;
}

} // namespace sub0pipeline::remote
