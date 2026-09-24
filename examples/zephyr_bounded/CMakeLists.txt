cmake_minimum_required(VERSION 3.21)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(Sub0PipelineZephyr LANGUAGES C CXX)
# Compile only the portable core. Requires a full C++23 standard library,
# including stop_token/mutex/thread; the minimal Zephyr C++ library is insufficient.
target_sources(app PRIVATE src/main.cpp ../../src/sub0pipeline.cpp)
target_include_directories(app PRIVATE ../../include)
target_compile_definitions(app PRIVATE SUB0PIPELINE_EXCEPTIONS=0)
