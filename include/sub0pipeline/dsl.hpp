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
//   #include "sub0pipeline/dsl.hpp"
//   using namespace sub0pipeline::dsl;
//   Pipeline pipe;
//   pipe >> "load"_job(loadData)
//        >> "parse"_job(parse).timeout(500ms) + "validate"_job(validate)
//        >> "commit"_job(commit);

#pragma once
#include "sub0pipeline/sub0pipeline.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace sub0pipeline::dsl
{

// ── Forward declarations ─────────────────────────────────────────────────────

template<typename F> class JobSpec;
template<typename... Fs> class JobSpecGroup;
template<std::size_t N> struct JobTuple;
template<typename... Layers> class JobTupleChain;

// ── JobNameProxy — returned by _job UDL ──────────────────────────────────────

/**
 * Carries a job name from the `_job` literal until a callable is attached.
 *
 * The name is borrowed, not copied: it must outlive the JobSpec built from it
 * (a string literal always does).
 */
struct JobNameProxy
{
    std::string_view name;

    /**
     * Attaches the callable, producing a named JobSpec.
     * @tparam F  The callable type; decayed before it is stored.
     * @param fn  The job function, as accepted by Pipeline::emplace().
     * @return    A JobSpec holding the name and the callable.
     */
    template<typename F>
    JobSpec<std::decay_t<F>> operator()(F&& fn) const
    {
        return JobSpec<std::decay_t<F>>{name, std::forward<F>(fn)};
    }
};

/**
 * Names a job: `"load"_job(loadData)` creates a JobSpec called "load".
 * @param str  The literal's characters. Borrowed.
 * @param len  The literal's length.
 * @return     A JobNameProxy to call with the job function.
 */
inline JobNameProxy operator""_job(const char* str, std::size_t len)
{
    return JobNameProxy{std::string_view{str, len}};
}

// ── JobSpec<F> — named job descriptor with builder methods ───────────────────

/**
 * A named job descriptor that is emplaced into a Pipeline later.
 *
 * Collects the name, function and job options, then applies them in build().
 * Satisfies the concept used by Pipeline::emplace(Spec). The name is borrowed.
 * @tparam F  The job function type.
 */
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
    /**
     * Creates a spec.
     * @param name  The job name, borrowed; may be empty for an unnamed job.
     * @param fn    The job function, as accepted by Pipeline::emplace().
     */
    JobSpec(std::string_view name, F fn)
        : name_{name}, fn_{std::move(fn)} {}

    /**
     * Sets the job timeout, as Job::timeout() does.
     * @param t  The maximum execution time; zero leaves it unset.
     * @return *this for chaining.
     */
    JobSpec& timeout(std::chrono::milliseconds t)  { timeout_ = t; return *this; }

    /**
     * Sets the priority hint, as Job::priority() does.
     * @param p  The priority; zero leaves it unset.
     * @return *this for chaining.
     */
    JobSpec& priority(uint8_t p)                   { priority_ = p; return *this; }

    /**
     * Sets the core affinity hint, as Job::core() does.
     * @param c  The core index; -1 leaves it unset.
     * @return *this for chaining.
     */
    JobSpec& core(int c)                           { coreAffinity_ = c; return *this; }

    /**
     * Sets the stack size hint, as Job::stack() does.
     * @param bytes  The stack size in bytes; zero leaves it unset.
     * @return *this for chaining.
     */
    JobSpec& stack(uint32_t bytes)                 { stackBytes_ = bytes; return *this; }

    /**
     * Marks the job optional, as Job::optional() does.
     * @param opt  true to mark the job optional.
     * @return *this for chaining.
     */
    JobSpec& optional(bool opt = true)             { isOptional_ = opt; return *this; }

    /**
     * Emplaces the job into @p p and applies the collected options.
     * Satisfies the Pipeline::emplace(Spec) concept.
     * @param p  The pipeline to add the job to.
     * @return    The new job handle.
     */
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

/**
 * Creates an unnamed JobSpec.
 * @tparam F  The callable type; decayed before it is stored.
 * @param fn  The job function, as accepted by Pipeline::emplace().
 * @return    A JobSpec with no name.
 */
template<typename F>
JobSpec<std::decay_t<F>> job(F&& fn)
{
    return JobSpec<std::decay_t<F>>{{}, std::forward<F>(fn)};
}

// ── JobSpecGroup<Fs...> — deferred parallel group (not yet emplaced) ─────────

/**
 * A deferred group of parallel JobSpecs that are not yet emplaced.
 *
 * Created by `JobSpec + JobSpec`; emplacing it creates independent jobs.
 * @tparam Fs  The job function types, one per spec.
 */
template<typename... Fs>
class JobSpecGroup
{
    std::tuple<JobSpec<Fs>...> specs_;

public:
    /**
     * Creates a group from its specs.
     * @param specs  The specs, in order.
     */
    explicit JobSpecGroup(JobSpec<Fs>... specs)
        : specs_{std::move(specs)...} {}

    /**
     * Emplaces every spec into the pipeline.
     * @param p  The pipeline to add the jobs to.
     * @return    A JobGroup of the new jobs, in spec order.
     */
    JobGroup buildAll(Pipeline& p) const
    {
        return std::apply(
            [&p](const auto&... specs) {
                JobGroup g;
                (g.add(specs.build(p)), ...);
                return g;
            },
            specs_);
    }

    /**
     * Gives access to the underlying tuple, for extending with operator+.
     * @return The specs, in order.
     */
    const auto& tuple() const { return specs_; }
};

// ── Helper to concatenate tuples into a new JobSpecGroup ─────────────────────

namespace detail
{

template<typename... Fs>
JobSpecGroup<Fs...> makeSpecGroup(JobSpec<Fs>... specs)
{
    return JobSpecGroup<Fs...>{std::move(specs)...};
}

template<typename Tuple, std::size_t... Is>
auto tupleToSpecGroup(Tuple&& t, std::index_sequence<Is...>)
{
    return makeSpecGroup(std::get<Is>(std::forward<Tuple>(t))...);
}

} // namespace detail

// ── JobTuple<N> — fixed-size job group with structured binding support ───────

/**
 * Holds a fixed-size group of Job handles and supports structured bindings.
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

    /**
     * Converts to a JobGroup for interop with the existing operators.
     * @return A JobGroup holding every job in the tuple.
     */
    operator JobGroup() const
    {
        JobGroup g;
        for (auto j : jobs) g.add(j);
        return g;
    }

    /**
     * Gives the pipeline the first member belongs to.
     * @return The pipeline, or nullptr for a default-constructed handle.
     */
    [[nodiscard]] Pipeline* pipeline() const { return jobs[0].pipeline(); }
};

// ── JobTupleChain<Layers...> — multi-layer capture ───────────────────────────

/**
 * Accumulates multiple JobTuple layers for layered structured bindings.
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
    /**
     * Creates a chain from its layers.
     * @param layers  The layers, first to last.
     */
    explicit JobTupleChain(Layers... layers)
        : layers_{std::move(layers)...} {}

    /**
     * Gives the last layer, which is the active front for wiring.
     * @return The last layer.
     */
    auto& last() { return std::get<sizeof...(Layers) - 1>(layers_); }

    /**
     * Gives the last layer, which is the active front for wiring.
     * @return The last layer, read-only.
     */
    const auto& last() const { return std::get<sizeof...(Layers) - 1>(layers_); }

    /**
     * Gives all layers, for structured bindings via the tuple protocol.
     * @return The layers, first to last.
     */
    const auto& tuple() const { return layers_; }

    /**
     * Gives the pipeline the last layer belongs to.
     * @return The pipeline.
     */
    [[nodiscard]] Pipeline* pipeline() const { return last().pipeline(); }

    /**
     * Appends a new layer, returning an extended chain.
     * @tparam M        The number of jobs in the new layer.
     * @param newLayer  The layer to append.
     * @return          A chain with @p newLayer as its last layer.
     */
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

namespace sub0pipeline::dsl
{

// ── get<> for JobTuple ───────────────────────────────────────────────────────

/**
 * Gets the I-th job of a tuple, for structured bindings.
 * @tparam I  Index of the job.
 * @tparam N  Number of jobs in the tuple.
 * @param t   The tuple.
 * @return    The I-th job.
 */
template<std::size_t I, std::size_t N>
Job get(JobTuple<N> const& t) { return t.jobs[I]; }

/**
 * Gets the I-th job of a tuple, for structured bindings.
 * @tparam I  Index of the job.
 * @tparam N  Number of jobs in the tuple.
 * @param t   The tuple.
 * @return    The I-th job.
 */
template<std::size_t I, std::size_t N>
Job get(JobTuple<N>& t) { return t.jobs[I]; }

/**
 * Gets the I-th job of a tuple, for structured bindings.
 * @tparam I  Index of the job.
 * @tparam N  Number of jobs in the tuple.
 * @param t   The tuple.
 * @return    The I-th job.
 */
template<std::size_t I, std::size_t N>
Job get(JobTuple<N>&& t) { return t.jobs[I]; }

// ── get<> for JobTupleChain ──────────────────────────────────────────────────

/**
 * Gets the I-th layer of a chain, for structured bindings.
 * @tparam I       Index of the layer.
 * @tparam Layers  The layer types.
 * @param c        The chain.
 * @return         The I-th layer.
 */
template<std::size_t I, typename... Layers>
auto get(JobTupleChain<Layers...> const& c)
    -> std::tuple_element_t<I, std::tuple<Layers...>>
{
    return std::get<I>(c.tuple());
}

/**
 * Gets the I-th layer of a chain, for structured bindings.
 * @tparam I       Index of the layer.
 * @tparam Layers  The layer types.
 * @param c        The chain.
 * @return         The I-th layer.
 */
template<std::size_t I, typename... Layers>
auto get(JobTupleChain<Layers...>& c)
    -> std::tuple_element_t<I, std::tuple<Layers...>>
{
    return std::get<I>(c.tuple());
}

/**
 * Gets the I-th layer of a chain, for structured bindings.
 * @tparam I       Index of the layer.
 * @tparam Layers  The layer types.
 * @param c        The chain.
 * @return         The I-th layer.
 */
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

/**
 * Sequential: @p lhs runs before @p rhs; returns @p rhs for left-associative chaining.
 * @param lhs The predecessor.
 * @param rhs The successor.
 * @return @p rhs.
 */
inline Job operator>>(Job lhs, Job rhs)
{
    lhs.precede(rhs);
    return rhs;
}

/**
 * Groups two jobs in parallel; no dependencies are created.
 * @param lhs The first member.
 * @param rhs The second member.
 * @return A JobGroup of both jobs.
 */
inline JobGroup operator+(Job lhs, Job rhs)
{
    return JobGroup{lhs, rhs};
}

/**
 * Adds a job to a parallel group; no dependencies are created.
 * @param lhs The group to extend; taken by value.
 * @param rhs The job to add.
 * @return The extended group.
 */
inline JobGroup operator+(JobGroup lhs, Job rhs)
{
    lhs.add(rhs);
    return lhs;
}

/**
 * Merges two parallel groups; no dependencies are created.
 * @param lhs The group to extend; taken by value.
 * @param rhs The jobs to add.
 * @return The merged group.
 */
inline JobGroup operator+(JobGroup lhs, JobGroup const& rhs)
{
    for (auto j : rhs.jobs()) lhs.add(j);
    return lhs;
}

/**
 * Makes @p lhs precede every job in @p rhs; returns @p rhs for chaining.
 * @param lhs The predecessor.
 * @param rhs The successors.
 * @return @p rhs.
 */
inline JobGroup operator>>(Job lhs, JobGroup rhs)
{
    for (auto j : rhs.jobs()) lhs.precede(j);
    return rhs;
}

/**
 * Makes every job in @p lhs precede @p rhs; returns @p rhs for chaining.
 * @param lhs The predecessors.
 * @param rhs The successor.
 * @return @p rhs.
 */
inline Job operator>>(JobGroup const& lhs, Job rhs)
{
    for (auto j : lhs.jobs()) j.precede(rhs);
    return rhs;
}

/**
 * Cross-product: every job in @p lhs precedes every job in @p rhs.
 * @param lhs The predecessors.
 * @param rhs The successors.
 * @return @p rhs.
 */
inline JobGroup operator>>(JobGroup const& lhs, JobGroup rhs)
{
    for (auto l : lhs.jobs())
        for (auto r : rhs.jobs())
            l.precede(r);
    return rhs;
}

// ── Pipe syntax: Pipeline/Job >> JobSpec (inline emplace + wire) ─────────────

/**
 * Emplaces @p spec into @p pipe.
 * @tparam F The spec's function type.
 * @param pipe The pipeline to add the job to.
 * @param spec The job to emplace.
 * @return The new job.
 */
template<typename F>
Job operator>>(Pipeline& pipe, JobSpec<F> spec)
{
    return spec.build(pipe);
}

/**
 * Emplaces @p rhs into the same pipeline and makes @p lhs precede it.
 * @tparam F The spec's function type.
 * @param lhs The predecessor.
 * @param rhs The job to emplace.
 * @return The new job.
 */
template<typename F>
Job operator>>(Job lhs, JobSpec<F> rhs)
{
    auto newJob = rhs.build(*lhs.pipeline());
    lhs.precede(newJob);
    return newJob;
}

/**
 * Emplaces every spec in @p rhs as independent jobs.
 * @tparam Fs The group's function types.
 * @param pipe The pipeline to add the jobs to.
 * @param rhs The specs to emplace.
 * @return A JobTuple of the new jobs, for structured bindings.
 */
template<typename... Fs>
auto operator>>(Pipeline& pipe, JobSpecGroup<Fs...> const& rhs)
{
    return std::apply(
        [&pipe](const auto&... specs) {
            return JobTuple<sizeof...(Fs)>{{specs.build(pipe)...}};
        },
        rhs.tuple());
}

/**
 * Emplaces every spec in @p rhs and makes @p lhs precede each.
 * @tparam Fs The group's function types.
 * @param lhs The predecessor.
 * @param rhs The specs to emplace.
 * @return A JobGroup of the new jobs.
 */
template<typename... Fs>
JobGroup operator>>(Job lhs, JobSpecGroup<Fs...> const& rhs)
{
    auto group = rhs.buildAll(*lhs.pipeline());
    for (auto j : group.jobs()) lhs.precede(j);
    return group;
}

/**
 * Emplaces every spec in @p rhs and makes every job in @p lhs precede each.
 * @tparam Fs The group's function types.
 * @param lhs The predecessors; must not be empty.
 * @param rhs The specs to emplace.
 * @return A JobGroup of the new jobs.
 */
template<typename... Fs>
JobGroup operator>>(JobGroup const& lhs, JobSpecGroup<Fs...> const& rhs)
{
    auto rhsGroup = rhs.buildAll(*lhs.jobs().front().pipeline());
    for (auto l : lhs.jobs())
        for (auto r : rhsGroup.jobs())
            l.precede(r);
    return rhsGroup;
}

/**
 * Emplaces @p rhs and makes every job in @p lhs precede it.
 * @tparam F The spec's function type.
 * @param lhs The predecessors; must not be empty.
 * @param rhs The job to emplace.
 * @return The new job.
 */
template<typename F>
Job operator>>(JobGroup const& lhs, JobSpec<F> rhs)
{
    // Use the pipeline from the first member of the group.
    auto newJob = rhs.build(*lhs.jobs().front().pipeline());
    for (auto j : lhs.jobs()) j.precede(newJob);
    return newJob;
}

// ── JobTuple >> operators (capture-preserving) ───────────────────────────────

/**
 * Makes every job in @p lhs precede @p rhs; returns @p lhs so it can still be captured.
 * @tparam N Number of jobs in the tuple.
 * @param lhs The predecessors.
 * @param rhs The successor.
 * @return @p lhs.
 */
template<std::size_t N>
JobTuple<N> operator>>(JobTuple<N> lhs, Job rhs)
{
    for (auto j : lhs.jobs) j.precede(rhs);
    return lhs;
}

/**
 * Emplaces @p rhs and makes every job in @p lhs precede it; returns @p lhs.
 * @tparam N Number of jobs in the tuple.
 * @tparam F The spec's function type.
 * @param lhs The predecessors.
 * @param rhs The job to emplace.
 * @return @p lhs.
 */
template<std::size_t N, typename F>
JobTuple<N> operator>>(JobTuple<N> lhs, JobSpec<F> rhs)
{
    auto newJob = rhs.build(*lhs.pipeline());
    for (auto j : lhs.jobs) j.precede(newJob);
    return lhs;
}

/**
 * Emplaces every spec in @p rhs and wires every job in @p lhs to each.
 * @tparam N Number of jobs in the tuple.
 * @tparam Fs The group's function types.
 * @param lhs The predecessors.
 * @param rhs The specs to emplace.
 * @return A JobTupleChain of @p lhs and the new layer.
 */
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

/**
 * Makes every job in the last layer of @p lhs precede @p rhs; returns @p lhs.
 * @tparam Layers The chain's layer types.
 * @param lhs The chain; its last layer is the predecessors.
 * @param rhs The successor.
 * @return @p lhs.
 */
template<typename... Layers>
JobTupleChain<Layers...> operator>>(JobTupleChain<Layers...> lhs, Job rhs)
{
    for (auto j : lhs.last().jobs) j.precede(rhs);
    return lhs;
}

/**
 * Emplaces @p rhs and makes every job in the last layer of @p lhs precede it; returns @p lhs.
 * @tparam Layers The chain's layer types.
 * @tparam F The spec's function type.
 * @param lhs The chain; its last layer is the predecessors.
 * @param rhs The job to emplace.
 * @return @p lhs.
 */
template<typename... Layers, typename F>
JobTupleChain<Layers...> operator>>(JobTupleChain<Layers...> lhs, JobSpec<F> rhs)
{
    auto newJob = rhs.build(*lhs.pipeline());
    for (auto j : lhs.last().jobs) j.precede(newJob);
    return lhs;
}

/**
 * Emplaces every spec in @p rhs, wires the last layer of @p lhs to each, and appends them as a layer.
 * @tparam Layers The chain's layer types.
 * @tparam Fs The group's function types.
 * @param lhs The chain; its last layer is the predecessors.
 * @param rhs The specs to emplace.
 * @return The chain extended by the new layer.
 */
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

/**
 * Groups two specs without emplacing them.
 * @tparam F1 The first spec's function type.
 * @tparam F2 The second spec's function type.
 * @param lhs The first spec.
 * @param rhs The second spec.
 * @return A JobSpecGroup of both specs.
 */
template<typename F1, typename F2>
auto operator+(JobSpec<F1> lhs, JobSpec<F2> rhs)
{
    return JobSpecGroup<F1, F2>{std::move(lhs), std::move(rhs)};
}

/**
 * Extends a spec group by one spec, without emplacing.
 * @tparam F The added spec's function type.
 * @tparam Fs The group's function types.
 * @param lhs The group to extend.
 * @param rhs The spec to add.
 * @return A JobSpecGroup with @p rhs appended.
 */
template<typename F, typename... Fs>
auto operator+(JobSpecGroup<Fs...> const& lhs, JobSpec<F> rhs)
{
    return std::apply(
        [&rhs](const auto&... existing) {
            return detail::makeSpecGroup(existing..., std::move(rhs));
        },
        lhs.tuple());
}

} // namespace sub0pipeline::dsl
