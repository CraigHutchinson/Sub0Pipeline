// Placeholder bodies so the remote tests build before the implementation.
#include "sub0pipeline/remote/registry.hpp"

namespace sub0pipeline::remote
{
uint64_t registryFingerprint(std::span<const JobKindId>) { return 0U; }
bool JobRegistry::add(JobKindId, JobKindFn) { return false; }
const JobKindFn* JobRegistry::find(std::string_view, uint32_t) const noexcept { return nullptr; }
std::vector<JobKindId> JobRegistry::kinds() const { return {}; }
uint64_t JobRegistry::fingerprint() const { return 0U; }
} // namespace sub0pipeline::remote
