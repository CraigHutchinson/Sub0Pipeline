// examples/minimal_pipeline/main.cpp
//
// Minimal Sub0Pipeline example: three jobs in a linear chain.
// Demonstrates emplace(), .name(), .succeed(), and run().

#include <sub0pipeline/sub0pipeline.hpp>
#include <cstdio>

// Forward-declared in sequential_executor.cpp (Sub0Pipeline::Headless).
namespace sub0pipeline { std::unique_ptr<IExecutor> makeSequentialExecutor(); }

int main()
{
    using namespace sub0pipeline;

    Pipeline pipeline;

    auto a = pipeline.emplace([] { std::printf("  [A] initialise\n"); }).name("A");
    auto b = pipeline.emplace([] { std::printf("  [B] configure\n"); }).name("B");
    auto c = pipeline.emplace([] { std::printf("  [C] start\n"); }).name("C");

    b.succeed(a);
    c.succeed(b);

    auto exec   = makeSequentialExecutor();
    auto result = pipeline.run(*exec);

    if (result) {
        std::printf("Pipeline completed successfully.\n");
    } else {
        std::printf("Pipeline failed.\n");
        return 1;
    }
    return 0;
}
