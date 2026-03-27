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
// Usage:
//   #include <sub0pipeline/dsl.hpp>
//   using namespace sub0pipeline::dsl;
//   Pipeline boot;
//   boot >> "nvs"_job(nvs_init)
//        >> "display"_job(display_init).timeout(500ms) + "network"_job(network_init)
//        >> "app"_job(app_start);

#pragma once
#include <sub0pipeline/sub0pipeline.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace sub0pipeline::dsl {

// ── Forward declarations ─────────────────────────────────────────────────────

template<typename F> class JobSpec;
template<typename... Fs> class JobSpecGroup;
template<std::size_t N> struct JobTuple;
template<typename... Layers> class JobTupleChain;

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

// ── JobTuple<N> — fixed-size job group with structured binding support ───────

/**
 * @brief A fixed-size group of Job handles supporting structured bindings.
 *
 * Produced by `Pipeline >> JobSpecGroup`. Subsequent `>>` operations wire
 * dependencies from the tuple's members but return `*this` (capture-preserving),
 * so the tuple can be captured via structured bindings at the end of the chain.
 *
 * @example auto [a, b, c] = pipe >> "A"_job(fn) + "B"_job(fn) + "C"_job(fn) >> sink;
 */
template<std::size_t N>
struct JobTuple
{
    std::array<Job, N> jobs{};

    /// Convert to JobGroup for interop with existing operators.
    operator JobGroup() const
    {
        JobGroup g;
        for (auto j : jobs) g.add(j);
        return g;
    }

    /// Access the pipeline from the first member.
    [[nodiscard]] Pipeline* pipeline() const { return jobs[0].pipeline(); }
};

// ── JobTupleChain<Layers...> — multi-layer capture ───────────────────────────

/**
 * @brief Accumulates multiple JobTuple layers for layered structured bindings.
 *
 * Produced when `JobTuple >> JobSpecGroup` (a second parallel layer is added).
 * Each `>>` appends a layer and wires the previous layer → new layer.
 * The last layer is the "active front" used for subsequent `>>` wiring.
 *
 * @example auto [l1, l2] = pipe >> a+b+c >> d+e+f >> sink;
 *          auto [a, b, c] = l1;
 *          auto [d, e, f] = l2;
 */
template<typename... Layers>
class JobTupleChain
{
    std::tuple<Layers...> layers_;

public:
    explicit JobTupleChain(Layers... layers)
        : layers_{std::move(layers)...} {}

    /// Access the last layer (the active front for wiring).
    auto& last() { return std::get<sizeof...(Layers) - 1>(layers_); }
    const auto& last() const { return std::get<sizeof...(Layers) - 1>(layers_); }

    /// Access all layers (for structured bindings via tuple protocol).
    const auto& tuple() const { return layers_; }

    /// Pipeline from the last layer.
    [[nodiscard]] Pipeline* pipeline() const { return last().pipeline(); }

    /// Append a new layer, returning an extended chain.
    template<std::size_t M>
    auto append(JobTuple<M> newLayer) const
    {
        return std::apply(
            [&newLayer](const auto&... existing) {
                return JobTupleChain<Layers..., JobTuple<M>>{existing..., std::move(newLayer)};
            },
            layers_);
    }
};

} // namespace sub0pipeline::dsl

// ── Tuple protocol for JobTuple (must be in namespace std) ───────────────────

template<std::size_t N>
struct std::tuple_size<sub0pipeline::dsl::JobTuple<N>>
    : std::integral_constant<std::size_t, N> {};

template<std::size_t I, std::size_t N>
struct std::tuple_element<I, sub0pipeline::dsl::JobTuple<N>>
{
    using type = sub0pipeline::Job;
};

// ── Tuple protocol for JobTupleChain ─────────────────────────────────────────

template<typename... Layers>
struct std::tuple_size<sub0pipeline::dsl::JobTupleChain<Layers...>>
    : std::integral_constant<std::size_t, sizeof...(Layers)> {};

template<std::size_t I, typename... Layers>
struct std::tuple_element<I, sub0pipeline::dsl::JobTupleChain<Layers...>>
{
    using type = std::tuple_element_t<I, std::tuple<Layers...>>;
};

namespace sub0pipeline::dsl {

// ── get<> for JobTuple ───────────────────────────────────────────────────────

template<std::size_t I, std::size_t N>
Job get(JobTuple<N> const& t) { return t.jobs[I]; }

template<std::size_t I, std::size_t N>
Job get(JobTuple<N>& t) { return t.jobs[I]; }

template<std::size_t I, std::size_t N>
Job get(JobTuple<N>&& t) { return t.jobs[I]; }

// ── get<> for JobTupleChain ──────────────────────────────────────────────────

template<std::size_t I, typename... Layers>
auto get(JobTupleChain<Layers...> const& c)
    -> std::tuple_element_t<I, std::tuple<Layers...>>
{
    return std::get<I>(c.tuple());
}

template<std::size_t I, typename... Layers>
auto get(JobTupleChain<Layers...>& c)
    -> std::tuple_element_t<I, std::tuple<Layers...>>
{
    return std::get<I>(c.tuple());
}

template<std::size_t I, typename... Layers>
auto get(JobTupleChain<Layers...>&& c)
    -> std::tuple_element_t<I, std::tuple<Layers...>>
{
    return std::get<I>(std::move(c).tuple());
}

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

/// Pipeline >> JobSpecGroup: emplace all (independent), return JobTuple.
template<typename... Fs>
auto operator>>(Pipeline& pipe, JobSpecGroup<Fs...> const& rhs)
{
    return std::apply(
        [&pipe](const auto&... specs) {
            return JobTuple<sizeof...(Fs)>{{specs.build(pipe)...}};
        },
        rhs.tuple());
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

// ── JobTuple >> operators (capture-preserving) ───────────────────────────────

/// JobTuple >> Job: wire all→rhs, return self (capture-preserving).
template<std::size_t N>
JobTuple<N> operator>>(JobTuple<N> lhs, Job rhs)
{
    for (auto j : lhs.jobs) j.precede(rhs);
    return lhs;
}

/// JobTuple >> JobSpec: emplace, wire all→new, return self.
template<std::size_t N, typename F>
JobTuple<N> operator>>(JobTuple<N> lhs, JobSpec<F> rhs)
{
    auto newJob = rhs.build(*lhs.pipeline());
    for (auto j : lhs.jobs) j.precede(newJob);
    return lhs;
}

/// JobTuple >> JobSpecGroup: emplace all, wire cross-product, return chain.
template<std::size_t N, typename... Fs>
auto operator>>(JobTuple<N> lhs, JobSpecGroup<Fs...> const& rhs)
{
    auto newLayer = std::apply(
        [&lhs](const auto&... specs) {
            return JobTuple<sizeof...(Fs)>{{specs.build(*lhs.pipeline())...}};
        },
        rhs.tuple());
    for (auto l : lhs.jobs)
        for (auto r : newLayer.jobs)
            l.precede(r);
    return JobTupleChain<JobTuple<N>, JobTuple<sizeof...(Fs)>>{std::move(lhs), std::move(newLayer)};
}

// ── JobTupleChain >> operators ───────────────────────────────────────────────

/// JobTupleChain >> Job: wire last layer→rhs, return self.
template<typename... Layers>
JobTupleChain<Layers...> operator>>(JobTupleChain<Layers...> lhs, Job rhs)
{
    for (auto j : lhs.last().jobs) j.precede(rhs);
    return lhs;
}

/// JobTupleChain >> JobSpec: emplace, wire last layer→new, return self.
template<typename... Layers, typename F>
JobTupleChain<Layers...> operator>>(JobTupleChain<Layers...> lhs, JobSpec<F> rhs)
{
    auto newJob = rhs.build(*lhs.pipeline());
    for (auto j : lhs.last().jobs) j.precede(newJob);
    return lhs;
}

/// JobTupleChain >> JobSpecGroup: emplace, wire last→new, append layer.
template<typename... Layers, typename... Fs>
auto operator>>(JobTupleChain<Layers...> lhs, JobSpecGroup<Fs...> const& rhs)
{
    auto newLayer = std::apply(
        [&lhs](const auto&... specs) {
            return JobTuple<sizeof...(Fs)>{{specs.build(*lhs.pipeline())...}};
        },
        rhs.tuple());
    for (auto l : lhs.last().jobs)
        for (auto r : newLayer.jobs)
            l.precede(r);
    return lhs.append(std::move(newLayer));
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
