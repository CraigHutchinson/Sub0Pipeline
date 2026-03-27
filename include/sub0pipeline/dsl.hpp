// include/sub0pipeline/dsl.hpp
//
// Sub0Pipeline DSL extension — optional operator-overloading layer.
//
// Provides expressive syntax for building pipeline DAGs using >> and +
// operators, the _job user-defined literal, and inline pipe syntax.
//
// All features live in sub0pipeline::dsl. A single
//   using namespace sub0pipeline::dsl;
// activates operators, the UDL, and helper types.
//
// Guard: all content is inside #if SUB0PIPELINE_ENABLE_DSL.
// Including this header without the flag compiles cleanly but does nothing.
//
// Usage:
//   #define SUB0PIPELINE_ENABLE_DSL 1
//   #include <sub0pipeline/dsl.hpp>
//
//   using namespace sub0pipeline::dsl;
//   Pipeline boot;
//   boot >> "nvs"_job(nvs_init)
//        >> "display"_job(display_init).timeout(500ms) + "network"_job(network_init)
//        >> "app"_job(app_start);

#pragma once
#include <sub0pipeline/sub0pipeline.hpp>

#include <chrono>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#if SUB0PIPELINE_ENABLE_DSL

namespace sub0pipeline::dsl {

// ── Forward declarations ─────────────────────────────────────────────────────

template<typename F> class JobSpec;
template<typename... Fs> class JobSpecGroup;

// ── JobNameProxy — returned by _job UDL ──────────────────────────────────────

struct JobNameProxy
{
    std::string_view name;

    template<typename F>
    JobSpec<std::decay_t<F>> operator()(F&& fn) const
    {
        return JobSpec<std::decay_t<F>>{name, std::forward<F>(fn)};
    }
};

inline JobNameProxy operator""_job(const char* str, std::size_t len)
{
    return JobNameProxy{std::string_view{str, len}};
}

// ── JobSpec<F> — named job descriptor with builder methods ───────────────────

template<typename F>
class JobSpec
{
    std::string_view            name_;
    F                           fn_;
    std::chrono::milliseconds   timeout_{0};
    uint8_t                     priority_{0};
    int                         coreAffinity_{-1};
    uint32_t                    stackBytes_{0};
    bool                        isOptional_{false};

public:
    JobSpec(std::string_view name, F fn)
        : name_{name}, fn_{std::move(fn)} {}

    JobSpec& timeout(std::chrono::milliseconds t)  { timeout_ = t; return *this; }
    JobSpec& priority(uint8_t p)                   { priority_ = p; return *this; }
    JobSpec& core(int c)                           { coreAffinity_ = c; return *this; }
    JobSpec& stack(uint32_t bytes)                 { stackBytes_ = bytes; return *this; }
    JobSpec& optional(bool opt = true)             { isOptional_ = opt; return *this; }

    /// Satisfies the Pipeline::emplace(Spec) concept.
    Job build(Pipeline& p) const
    {
        auto j = p.emplace(fn_);
        if (!name_.empty())     j.name(name_);
        if (timeout_.count() > 0) j.timeout(timeout_);
        if (priority_ > 0)     j.priority(priority_);
        if (coreAffinity_ >= 0) j.core(coreAffinity_);
        if (stackBytes_ > 0)   j.stack(stackBytes_);
        if (isOptional_)        j.optional();
        return j;
    }
};

// ── job() — unnamed job factory ──────────────────────────────────────────────

template<typename F>
JobSpec<std::decay_t<F>> job(F&& fn)
{
    return JobSpec<std::decay_t<F>>{{}, std::forward<F>(fn)};
}

// ── JobSpecGroup<Fs...> — deferred parallel group (not yet emplaced) ─────────

template<typename... Fs>
class JobSpecGroup
{
    std::tuple<JobSpec<Fs>...> specs_;

public:
    explicit JobSpecGroup(JobSpec<Fs>... specs)
        : specs_{std::move(specs)...} {}

    /// Emplace all specs into the pipeline, return a JobGroup.
    JobGroup build_all(Pipeline& p) const
    {
        return std::apply(
            [&p](const auto&... specs) {
                JobGroup g;
                (g.add(specs.build(p)), ...);
                return g;
            },
            specs_);
    }

    /// Access the underlying tuple (for extending with operator+).
    const auto& tuple() const { return specs_; }
};

// ── Helper to concatenate tuples into a new JobSpecGroup ─────────────────────

namespace detail {

template<typename... Fs>
JobSpecGroup<Fs...> make_spec_group(JobSpec<Fs>... specs)
{
    return JobSpecGroup<Fs...>{std::move(specs)...};
}

template<typename Tuple, std::size_t... Is>
auto tuple_to_spec_group(Tuple&& t, std::index_sequence<Is...>)
{
    return make_spec_group(std::get<Is>(std::forward<Tuple>(t))...);
}

} // namespace detail

// ═════════════════════════════════════════════════════════════════════════════
// Operators
// ═════════════════════════════════════════════════════════════════════════════

// ── Job/JobGroup operators (emplaced jobs) ────────────────────────────────────

/// Sequential: a runs before b; returns b for left-assoc chaining.
inline Job operator>>(Job lhs, Job rhs)
{
    lhs.precede(rhs);
    return rhs;
}

/// Parallel group: no dependencies created, just grouping.
inline JobGroup operator+(Job lhs, Job rhs)
{
    return JobGroup{lhs, rhs};
}

inline JobGroup operator+(JobGroup lhs, Job rhs)
{
    lhs.add(rhs);
    return lhs;
}

inline JobGroup operator+(JobGroup lhs, JobGroup const& rhs)
{
    for (auto j : rhs.jobs()) lhs.add(j);
    return lhs;
}

/// Job precedes every job in group; returns group for chaining.
inline JobGroup operator>>(Job lhs, JobGroup rhs)
{
    for (auto j : rhs.jobs()) lhs.precede(j);
    return rhs;
}

/// Every job in group precedes rhs; returns rhs for chaining.
inline Job operator>>(JobGroup const& lhs, Job rhs)
{
    for (auto j : lhs.jobs()) j.precede(rhs);
    return rhs;
}

/// Cross-product: every job in lhs precedes every job in rhs.
inline JobGroup operator>>(JobGroup const& lhs, JobGroup rhs)
{
    for (auto l : lhs.jobs())
        for (auto r : rhs.jobs())
            l.precede(r);
    return rhs;
}

// ── Pipe syntax: Pipeline/Job >> JobSpec (inline emplace + wire) ─────────────

/// Pipeline >> JobSpec: emplace into pipeline, return Job.
template<typename F>
Job operator>>(Pipeline& pipe, JobSpec<F> spec)
{
    return spec.build(pipe);
}

/// Job >> JobSpec: emplace into same pipeline, wire lhs→new, return new Job.
template<typename F>
Job operator>>(Job lhs, JobSpec<F> rhs)
{
    auto newJob = rhs.build(*lhs.pipeline());
    lhs.precede(newJob);
    return newJob;
}

/// Pipeline >> JobSpecGroup: emplace all (independent), return JobGroup.
template<typename... Fs>
JobGroup operator>>(Pipeline& pipe, JobSpecGroup<Fs...> const& rhs)
{
    return rhs.build_all(pipe);
}

/// Job >> JobSpecGroup: emplace all, wire lhs→each, return JobGroup.
template<typename... Fs>
JobGroup operator>>(Job lhs, JobSpecGroup<Fs...> const& rhs)
{
    auto group = rhs.build_all(*lhs.pipeline());
    for (auto j : group.jobs()) lhs.precede(j);
    return group;
}

/// JobGroup >> JobSpecGroup: emplace all, wire each-in-lhs→each-in-rhs, return new JobGroup.
template<typename... Fs>
JobGroup operator>>(JobGroup const& lhs, JobSpecGroup<Fs...> const& rhs)
{
    auto rhsGroup = rhs.build_all(*lhs.jobs().front().pipeline());
    for (auto l : lhs.jobs())
        for (auto r : rhsGroup.jobs())
            l.precede(r);
    return rhsGroup;
}

/// JobGroup >> JobSpec: emplace, wire each→new, return new Job.
template<typename F>
Job operator>>(JobGroup const& lhs, JobSpec<F> rhs)
{
    // Use the pipeline from the first member of the group.
    auto newJob = rhs.build(*lhs.jobs().front().pipeline());
    for (auto j : lhs.jobs()) j.precede(newJob);
    return newJob;
}

// ── JobSpec grouping (deferred, not yet emplaced) ────────────────────────────

/// JobSpec + JobSpec: group two specs (no emplacement).
template<typename F1, typename F2>
auto operator+(JobSpec<F1> lhs, JobSpec<F2> rhs)
{
    return JobSpecGroup<F1, F2>{std::move(lhs), std::move(rhs)};
}

/// JobSpecGroup + JobSpec: extend group.
template<typename F, typename... Fs>
auto operator+(JobSpecGroup<Fs...> const& lhs, JobSpec<F> rhs)
{
    return std::apply(
        [&rhs](const auto&... existing) {
            return detail::make_spec_group(existing..., std::move(rhs));
        },
        lhs.tuple());
}

} // namespace sub0pipeline::dsl

#endif // SUB0PIPELINE_ENABLE_DSL
