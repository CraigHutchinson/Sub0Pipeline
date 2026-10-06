#include "sub0pipeline/sub0pipeline.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iostream>
#include <span>
#include <string_view>
#include <thread>


namespace
{

using Clock = std::chrono::steady_clock;

/// Scheduler signals retained by the bounded recorder.
enum class EventKind : std::uint8_t { Start, Finish, Dependency };

/// Display names borrow the unchanged Pipeline until serialization completes.
struct TraceEvent
{
    EventKind kind{};
    sub0pipeline::RunId runId{};
    sub0pipeline::JobId jobId{};
    sub0pipeline::JobId otherJobId{};
    std::string_view name{};
    std::string_view otherName{};
    sub0pipeline::JobStatus status{sub0pipeline::JobStatus::kPending};
    float progress{};
    std::int64_t timestampUs{};
    std::uint64_t threadId{};
};

void writeJsonString(std::ostream& output, std::string_view value)
{
    constexpr char hex[] = "0123456789abcdef";
    output.put('"');
    for (const unsigned char ch : value)
    {
        switch (ch)
        {
            case '"': output << "\\\""; break;
            case '\\': output << "\\\\"; break;
            case '\b': output << "\\b"; break;
            case '\f': output << "\\f"; break;
            case '\n': output << "\\n"; break;
            case '\r': output << "\\r"; break;
            case '\t': output << "\\t"; break;
            default:
                if (ch < 0x20U)
                {
                    output << "\\u00" << hex[ch >> 4U] << hex[ch & 0x0fU];
                }
                else
                {
                    output.put(static_cast<char>(ch));
                }
        }
    }
    output.put('"');
}

/** Fixed-capacity concurrent recorder; overflow drops events.
 * Read and serialize only after all producer callbacks have joined.
 */
class TraceRecorder final : public sub0pipeline::IObserver
{
public:
    sub0pipeline::RunId onRunStart() override
    {
        return nextRunId_.fetch_add(1U, std::memory_order_relaxed) + 1U;
    }

    void onJobStart(sub0pipeline::RunId runId, sub0pipeline::JobId jobId,
                    std::string_view name) override
    {
        record({EventKind::Start, runId, jobId, 0U, name});
    }

    void onJobFinish(sub0pipeline::RunId runId, sub0pipeline::JobId jobId,
                     std::string_view name, sub0pipeline::JobStatus status,
                     float progress) override
    {
        record({EventKind::Finish, runId, jobId, 0U, name, {}, status, progress});
    }

    void onDependenciesResolved(sub0pipeline::RunId runId,
                                sub0pipeline::JobId from,
                                std::string_view fromName,
                                sub0pipeline::DependencyRange successors) override
    {
        for (const auto target : successors)
        {
            record({EventKind::Dependency, runId, from, target.id,
                    fromName, target.name});
        }
    }

    [[nodiscard]] std::size_t size() const noexcept
    {
        const auto claimed = nextEvent_.load(std::memory_order_relaxed);
        return claimed < events_.size() ? claimed : events_.size();
    }

    [[nodiscard]] std::size_t dropped() const noexcept
    {
        return dropped_.load(std::memory_order_relaxed);
    }

    void writeChromeTrace(std::ostream& output) const
    {
        output << "{\"traceEvents\":[";
        bool first = true;
        for (const auto& event : std::span{events_}.first(size()))
        {
            if (!first) output.put(',');
            first = false;
            output << "{\"name\":";
            writeJsonString(output, event.kind == EventKind::Dependency
                ? std::string_view{"dependency"} : event.name);
            output << ",\"cat\":\"pipeline\",\"ph\":\"";
            if (event.kind == EventKind::Start) output << 'B';
            else if (event.kind == EventKind::Finish &&
                     event.status != sub0pipeline::JobStatus::kSkipped) output << 'E';
            else output << 'i';
            output << '"';
            if (event.kind == EventKind::Dependency ||
                (event.kind == EventKind::Finish &&
                 event.status == sub0pipeline::JobStatus::kSkipped))
                output << ",\"s\":\"t\"";
            output << ",\"ts\":" << event.timestampUs
                   << ",\"pid\":1,\"tid\":" << event.threadId
                   << ",\"args\":{\"run_id\":" << event.runId
                   << ",\"job_id\":" << event.jobId;
            if (event.kind == EventKind::Finish)
            {
                output << ",\"status\":"
                       << static_cast<unsigned>(event.status)
                       << ",\"progress\":" << event.progress;
            }
            else if (event.kind == EventKind::Dependency)
            {
                output << ",\"to_job_id\":" << event.otherJobId
                       << ",\"from\":";
                writeJsonString(output, event.name);
                output << ",\"to\":";
                writeJsonString(output, event.otherName);
            }
            output << "}}";
        }
        output << "]}\n";
    }

private:
    void record(TraceEvent event) noexcept
    {
        const auto index = nextEvent_.fetch_add(1U, std::memory_order_relaxed);
        if (index >= events_.size())
        {
            dropped_.fetch_add(1U, std::memory_order_relaxed);
            return;
        }
        event.timestampUs = std::chrono::duration_cast<std::chrono::microseconds>(
            Clock::now().time_since_epoch()).count();
        event.threadId = std::hash<std::thread::id>{}(std::this_thread::get_id());
        events_[index] = event;
    }

    std::array<TraceEvent, 32> events_{};
    std::atomic<std::size_t> nextEvent_{};
    std::atomic<std::size_t> dropped_{};
    std::atomic<sub0pipeline::RunId> nextRunId_{};
};

auto work() -> std::expected<void, sub0pipeline::PipelineError>
{
    std::this_thread::sleep_for(std::chrono::milliseconds{40});
    return {};
}

} // namespace

int main(int argc, char* argv[])
{
    using namespace sub0pipeline;

    const std::string_view scenario = argc == 2 ? argv[1] : "diamond";
    if (argc > 2 || (scenario != "diamond" && scenario != "boot" && scenario != "failure"))
    {
        std::cerr << "Usage: trace_capture [diamond|boot|failure]\n";
        return 1;
    }

    Pipeline pipeline;
    std::size_t expectedEvents{};
    if (scenario == "diamond")
    {
        auto root = pipeline.emplace(work).name("root");
        auto left = pipeline.emplace(work).name("left");
        auto right = pipeline.emplace(work).name("right");
        auto join = pipeline.emplace(work).name("join");
        left.succeed(root);
        right.succeed(root);
        join.succeed(left, right);
        expectedEvents = 12U;
    }
    else if (scenario == "boot")
    {
        auto storage = pipeline.emplace(work).name("storage");
        auto network = pipeline.emplace(work).name("network");
        auto display = pipeline.emplace(work).name("display");
        auto telemetry = pipeline.emplace(work).name("telemetry");
        auto controls = pipeline.emplace(work).name("controls");
        auto ready = pipeline.emplace(work).name("ready");
        network.succeed(storage);
        display.succeed(storage);
        telemetry.succeed(network);
        controls.succeed(display);
        ready.succeed(telemetry, controls);
        expectedEvents = 18U;
    }
    else
    {
        auto validate = pipeline.emplace(work).name("validate");
        auto commit = pipeline.emplace([]() -> std::expected<void, PipelineError> {
            std::this_thread::sleep_for(std::chrono::milliseconds{40});
            Pipeline::setCurrentJobError("simulated required commit failure");
            return std::unexpected(PipelineError::kJobFailed);
        }).name("commit");
        auto acknowledge = pipeline.emplace(work).name("acknowledge");
        auto publish = pipeline.emplace(work).name("publish");
        commit.succeed(validate);
        acknowledge.succeed(commit);
        publish.succeed(acknowledge);
        expectedEvents = 9U;
    }

    DesktopExecutor executor;
    TraceRecorder recorder;
    std::atomic<bool> finished{false};
    std::atomic<bool> expectedOutcome{false};
    std::jthread runner([&] {
        const auto result = pipeline.run(executor, &recorder);
        expectedOutcome.store(scenario == "failure"
            ? !result && result.error() == PipelineError::kJobFailed
            : result.has_value(), std::memory_order_relaxed);
        finished.store(true, std::memory_order_release);
    });

    do
    {
        for (const auto& job : pipeline.snapshot())
        {
            std::cerr << job.name << ": " << static_cast<unsigned>(job.status) << '\n';
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{16});
    } while (!finished.load(std::memory_order_acquire));
    runner.join();

    recorder.writeChromeTrace(std::cout);
    return expectedOutcome.load(std::memory_order_relaxed) &&
           recorder.size() == expectedEvents && recorder.dropped() == 0U &&
           std::cout.good() ? 0 : 1;
}
