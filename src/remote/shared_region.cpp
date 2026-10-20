// src/remote/shared_region.cpp
//
// Named shared-memory regions for the "shm" payload strategy.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <memory>
#include <random>
#include <string>

#include "shared_region.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace sub0pipeline::remote
{

namespace
{

// Process id, a per-process counter and a random suffix: unique across
// processes and across restarts that reuse a process id. Short enough for
// macOS, which limits POSIX shared-memory names to 31 characters.
std::string uniqueName()
{
    static std::atomic<uint32_t> counter{0U};
    std::random_device random;
    char suffix[9];
    const auto r = random();
#if defined(_WIN32)
    const auto pid = static_cast<unsigned long>(GetCurrentProcessId());
#else
    const auto pid = static_cast<unsigned long>(::getpid());
#endif
    std::snprintf(suffix, sizeof(suffix), "%08x", static_cast<unsigned>(r));
    std::string base = "s0pl-" + std::to_string(pid) + "-" + std::to_string(counter.fetch_add(1U)) + "-" + suffix;
#if defined(_WIN32)
    return "Local\\" + base;
#else
    return "/" + base.substr(0U, 30U);
#endif
}

} // namespace

std::unique_ptr<SharedRegion> SharedRegion::create(std::size_t size)
{
    std::unique_ptr<SharedRegion> region{new SharedRegion};
    region->name_ = uniqueName();
    region->size_ = size;
    region->creator_ = true;
#if defined(_WIN32)
    const std::wstring wide(region->name_.begin(), region->name_.end());
    const auto bytes = static_cast<unsigned long long>(size);
    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                        static_cast<DWORD>(bytes >> 32U), static_cast<DWORD>(bytes & 0xFFFFFFFFULL),
                                        wide.c_str());
    if (!mapping) return nullptr;
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        CloseHandle(mapping);
        return nullptr;
    }
    region->mapping_ = mapping;
    region->data_ = static_cast<std::byte*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, size));
    if (!region->data_) return nullptr;
#else
    const int fd = ::shm_open(region->name_.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) return nullptr;
    if (::ftruncate(fd, static_cast<off_t>(size)) != 0)
    {
        ::close(fd);
        ::shm_unlink(region->name_.c_str());
        return nullptr;
    }
    void* data = ::mmap(nullptr, size == 0U ? 1U : size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ::close(fd);
    if (data == MAP_FAILED)
    {
        ::shm_unlink(region->name_.c_str());
        return nullptr;
    }
    region->data_ = static_cast<std::byte*>(data);
#endif
    return region;
}

std::unique_ptr<SharedRegion> SharedRegion::open(const std::string& name, std::size_t size)
{
    std::unique_ptr<SharedRegion> region{new SharedRegion};
    region->name_ = name;
    region->size_ = size;
#if defined(_WIN32)
    const std::wstring wide(name.begin(), name.end());
    HANDLE mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, wide.c_str());
    if (!mapping) return nullptr;
    region->mapping_ = mapping;
    region->data_ = static_cast<std::byte*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, size));
    if (!region->data_) return nullptr;
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(region->data_, &info, sizeof(info)) == 0 || info.RegionSize < size) return nullptr;
#else
    const int fd = ::shm_open(name.c_str(), O_RDWR, 0600);
    if (fd < 0) return nullptr;
    struct stat st{};
    if (::fstat(fd, &st) != 0 || static_cast<std::size_t>(st.st_size) < size)
    {
        ::close(fd);
        return nullptr;
    }
    void* data = ::mmap(nullptr, size == 0U ? 1U : size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ::close(fd);
    if (data == MAP_FAILED) return nullptr;
    region->data_ = static_cast<std::byte*>(data);
#endif
    return region;
}

SharedRegion::~SharedRegion()
{
#if defined(_WIN32)
    if (data_) UnmapViewOfFile(data_);
    if (mapping_) CloseHandle(static_cast<HANDLE>(mapping_));
#else
    if (data_) ::munmap(data_, size_ == 0U ? 1U : size_);
    if (creator_) unlink();
#endif
}

void SharedRegion::unlink() noexcept
{
#if !defined(_WIN32)
    if (!unlinked_)
    {
        ::shm_unlink(name_.c_str());
        unlinked_ = true;
    }
#endif
}

} // namespace sub0pipeline::remote
