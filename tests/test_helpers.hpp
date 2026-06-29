#pragma once
// tests/test_helpers.hpp
//
// Shared test helpers -- InlineExecutor and RecordingExecutor used across
// multiple test translation units.

#include <sub0pipeline/sub0pipeline.hpp>

#include <string>
#include <vector>

namespace sub0pipeline
{

class InlineExecutor final : public IExecutor
{
public:
    void dispatch(std::string_view, std::function<void()> fn,
                  std::function<void()> onComplete,
                  int, uint8_t, uint32_t) override
    {
        fn();
        if (onComplete) onComplete();
    }
    void wait_all() override {}
    [[nodiscard]] int concurrency() const noexcept override { return 1; }
};

/// Sequential executor that records dispatch order before execution.
class RecordingExecutor final : public IExecutor
{
public:
    void dispatch(
        std::string_view              name,
        std::function<void()>         fn,
        std::function<void()>         onComplete,
        int                           /*core*/,
        uint8_t                       /*priority*/,
        uint32_t                      /*stack_bytes*/) override
    {
        order_.emplace_back(name);  // capture BEFORE execution
        fn();
        if (onComplete) onComplete();
    }

    void wait_all() override {}
    [[nodiscard]] int concurrency() const noexcept override { return 1; }

    [[nodiscard]] const std::vector<std::string>& order() const { return order_; }
    void clear() { order_.clear(); }

private:
    std::vector<std::string> order_;
};

} // namespace sub0pipeline
