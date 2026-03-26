#pragma once
// tests/test_helpers.hpp
//
// Shared test helpers — InlineExecutor used across multiple test translation units.

#include <sub0pipeline/sub0pipeline.hpp>

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

} // namespace sub0pipeline
