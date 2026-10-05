// include/sub0pipeline/sub0pipeline.hpp
//
// Sub0Pipeline — Lightweight C++23 DAG job scheduler.
//
// Declare jobs and their dependencies as a directed acyclic graph, then
// execute them in parallel via a platform-injectable executor. Independent
// jobs run concurrently; dependent jobs wait for their predecessors.
//
// Design principles:
//   - Graph-as-value: the DAG is a first-class inspectable object
//   - Builder pattern: fluent .precede()/.name() chaining on Job handles
//   - Observer hooks: opt-in run/job/dependency events for profiling and progress
//   - Platform-injectable executor: pluggable backends (threaded, sequential, RTOS)
//   - Zero-overhead when jobs are constexpr-declared
//
// This is the umbrella header: it includes the whole core API. Each part is also
// available on its own (pipeline.hpp, job.hpp, job_group.hpp, executor.hpp,
// scoped_executor.hpp, executor_factory.hpp, observer.hpp, dependency_range.hpp,
// tick_job.hpp, error.hpp, config.hpp). Optional layers stay opt-in: dsl.hpp,
// deadline.hpp, run_scope.hpp.
//
// Usage:
//   sub0pipeline::Pipeline pipe;
//   auto a = pipe.emplace([] { return init_a(); }).name("A");
//   auto b = pipe.emplace([] { return init_b(); }).name("B").timeout(8s);
//   auto c = pipe.emplace([] { return init_c(); }).name("C").timeout(10s);
//   auto d = pipe.emplace([] { return start_d(); }).name("D");
//   d.succeed(b, c);   // D depends on both B and C
//   // B and C have no mutual dependency — run in parallel
//   pipe.run(executor, &observer);
//
#pragma once

#include <sub0pipeline/config.hpp>
#include <sub0pipeline/dependency_range.hpp>
#include <sub0pipeline/error.hpp>
#include <sub0pipeline/executor.hpp>
#include <sub0pipeline/executor_factory.hpp>
#include <sub0pipeline/job.hpp>
#include <sub0pipeline/job_group.hpp>
#include <sub0pipeline/observer.hpp>
#include <sub0pipeline/pipeline.hpp>
#include <sub0pipeline/scoped_executor.hpp>
#include <sub0pipeline/tick_job.hpp>
