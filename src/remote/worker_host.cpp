// Placeholder bodies so the remote tests build before the implementation.
#include "sub0pipeline/remote/worker_host.hpp"

namespace sub0pipeline::remote
{
struct WorkerHost::Work {};
WorkerHost::WorkerHost(const JobRegistry& registry, ITransport& transport, Options options)
    : registry_{registry}, transport_{transport}, options_{options} {}
WorkerHost::~WorkerHost() = default;
void WorkerHost::serve(std::stop_token stop)
{
    std::unique_lock lock{mtx_};
    changed_.wait(lock, stop, [] { return false; });
}
void WorkerHost::onFrame(std::span<const std::byte>) {}
void WorkerHost::onClosed(TransportError) noexcept {}
void WorkerHost::runner(std::stop_token) {}
void WorkerHost::run(Work&) {}
} // namespace sub0pipeline::remote
