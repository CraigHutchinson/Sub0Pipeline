// platform/headless/sequential_executor.cpp
//
// Factory for SequentialExecutor, which is itself header-only.

#include <sub0pipeline/executor/sequential_executor.hpp>

namespace sub0pipeline {

std::unique_ptr<IExecutor> makeSequentialExecutor()
{
    return std::make_unique<SequentialExecutor>();
}

} // namespace sub0pipeline
