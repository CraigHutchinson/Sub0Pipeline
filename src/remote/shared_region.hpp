// src/remote/shared_region.hpp
//
// A named shared-memory region: a Windows named file mapping or a POSIX
// shm_open segment, mapped read-write.
#pragma once

#include <cstddef>
#include <memory>
#include <string>

namespace sub0pipeline::remote
{

class SharedRegion
{
public:
    /**
     * Creates a new region under a unique name.
     * @param size  Bytes; the mapping is at least this large.
     * @return The region, or nullptr if the platform refused.
     */
    [[nodiscard]] static std::unique_ptr<SharedRegion> create(std::size_t size);

    /**
     * Maps an existing region by name.
     * @param name  The creator's name().
     * @param size  The creator's size(); a smaller segment is refused.
     * @return The region, or nullptr if it cannot be opened from this process.
     */
    [[nodiscard]] static std::unique_ptr<SharedRegion> open(const std::string& name, std::size_t size);

    /** Unmaps; the creator also removes the name if unlink() was not called. */
    ~SharedRegion();

    SharedRegion(const SharedRegion&)            = delete;
    SharedRegion& operator=(const SharedRegion&) = delete;

    [[nodiscard]] std::byte* data() const noexcept { return data_; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] const std::string& name() const noexcept { return name_; }

    /**
     * Removes the name so the segment lives only while mapped (POSIX); a
     * no-op on Windows, which frees a mapping when its last handle closes.
     */
    void unlink() noexcept;

private:
    SharedRegion() = default;

    std::byte*  data_{nullptr};
    std::size_t size_{0U};
    std::string name_;
    bool        creator_{false};
    bool        unlinked_{false};
#if defined(_WIN32)
    void*       mapping_{nullptr}; // HANDLE
#endif
};

} // namespace sub0pipeline::remote
