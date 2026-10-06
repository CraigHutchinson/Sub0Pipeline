// include/sub0pipeline/dependency_range.hpp
//
// DependencyRange — non-owning view of a node's successors.
#pragma once

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string_view>

#include "sub0pipeline/job.hpp"

namespace sub0pipeline
{

class Pipeline;

/** A non-owning range of successor identities. Iterators borrow the graph,
 * independently of the range wrapper; graph edits or destruction invalidate them.
 */
class DependencyRange
{
public:
    DependencyRange() noexcept = default;

    /// One outgoing edge's destination and borrowed display name.
    struct Target
    {
        JobId id{};
        std::string_view name{};
    };

    /// Input iterator borrowing the Pipeline and its successor storage.
    class Iterator
    {
    public:
        using value_type = Target;
        using difference_type = std::ptrdiff_t;
        using iterator_concept = std::input_iterator_tag;
        using iterator_category = std::input_iterator_tag;

        Iterator() noexcept = default;
        [[nodiscard]] Target operator*() const noexcept;
        Iterator& operator++() noexcept { ++index_; return *this; }
        Iterator operator++(int) noexcept
        {
            auto previous = *this;
            ++index_;
            return previous;
        }
        [[nodiscard]] bool operator==(const Iterator&) const noexcept = default;

    private:
        friend class DependencyRange;
        Iterator(const Pipeline* pipeline, const std::uint16_t* ids,
                 std::size_t index) noexcept
            : pipeline_{pipeline}, ids_{ids}, index_{index} {}

        const Pipeline* pipeline_{}; // non-owning
        const std::uint16_t* ids_{}; // non-owning
        std::size_t index_{};
    };

    /**
     * Returns an iterator to the first successor.
     * @return The iterator; equal to end() when the range is empty.
     */
    [[nodiscard]] Iterator begin() const noexcept { return Iterator{pipeline_, ids_, 0U}; }

    /**
     * Returns the iterator one past the last successor.
     * @return The end iterator.
     */
    [[nodiscard]] Iterator end() const noexcept { return Iterator{pipeline_, ids_, size_}; }

    /**
     * Returns the number of successors.
     * @return The successor count.
     */
    [[nodiscard]] std::size_t size() const noexcept { return size_; }

    /**
     * Reports whether the range has no successors.
     * @return true if size() is zero.
     */
    [[nodiscard]] bool empty() const noexcept { return size_ == 0U; }

private:
    friend class Pipeline;
    DependencyRange(const Pipeline* pipeline, const std::uint16_t* ids,
                    std::size_t size) noexcept
        : pipeline_{pipeline}, ids_{ids}, size_{size} {}

    const Pipeline* pipeline_{}; // non-owning
    const std::uint16_t* ids_{}; // non-owning
    std::size_t size_{};
};

} // namespace sub0pipeline
