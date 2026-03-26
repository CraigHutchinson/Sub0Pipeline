// platform/headless/sequential_executor.cpp
//
// Inline (sequential) executor for Sub0Pipeline.
// Runs each dispatched job synchronously on the calling thread with no
// parallelism. Deterministic execution order makes this ideal for unit tests
// and environments where std::thread is unavailable (bare-metal, CI).

#include <sub0pipeline/sub0pipeline.hpp>

#include <memory>

namespace sub0pipeline {

class SequentialExecutor final : public IExecutor
{
public:
    void dispatch(
        std::string_view              /*name*/,
        std::function<void()>         fn,
        std::function<void()>         on_complete,
        int                           /*core*/,
        uint8_t                       /*priority*/,
        uint32_t                      /*stack_bytes*/) override
    {
        fn();
        if (on_complete) on_complete();
    }

    void wait_all() override
    {
        // Nothing to wait for — all jobs run inline in dispatch().
    }

    [[nodiscard]] int concurrency() const noexcept override { return 1; }
};

/** @return A SequentialExecutor that runs all jobs inline (no threads). */
std::unique_ptr<IExecutor> make_sequential_executor()
{
    return std::make_unique<SequentialExecutor>();
}

} // namespace sub0pipeline
