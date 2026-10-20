// src/remote/pipe_transport.cpp
//
// PipeTransport: frames over a pair of byte-stream handles.

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <utility>

#include "sub0pipeline/remote/pipe_transport.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#endif

namespace sub0pipeline::remote
{

namespace
{

// Rejects a corrupt length before it becomes a huge allocation.
constexpr uint32_t kMaxBodyBytes = 0x7FFFFFFFU;

#if defined(_WIN32)

HANDLE toHandle(PipeTransport::NativeHandle h) noexcept { return reinterpret_cast<HANDLE>(h); }

bool writeAll(PipeTransport::NativeHandle handle, std::span<const std::byte> bytes) noexcept
{
    while (!bytes.empty())
    {
        const auto chunk = static_cast<DWORD>(std::min<std::size_t>(bytes.size(), 1U << 30U));
        DWORD written = 0;
        if (!WriteFile(toHandle(handle), bytes.data(), chunk, &written, nullptr) || written == 0) return false;
        bytes = bytes.subspan(written);
    }
    return true;
}

// Reads exactly bytes.size() bytes; false on end of stream, error or cancellation.
bool readAll(PipeTransport::NativeHandle handle, PipeTransport::NativeHandle, std::span<std::byte> bytes) noexcept
{
    while (!bytes.empty())
    {
        const auto chunk = static_cast<DWORD>(std::min<std::size_t>(bytes.size(), 1U << 30U));
        DWORD got = 0;
        if (!ReadFile(toHandle(handle), bytes.data(), chunk, &got, nullptr) || got == 0) return false;
        bytes = bytes.subspan(got);
    }
    return true;
}

void closeHandle(PipeTransport::NativeHandle& handle) noexcept
{
    if (handle != -1 && handle != 0)
    {
        CloseHandle(toHandle(handle));
        handle = -1;
    }
}

#else

bool writeAll(PipeTransport::NativeHandle handle, std::span<const std::byte> bytes) noexcept
{
    while (!bytes.empty())
    {
        const auto n = ::write(static_cast<int>(handle), bytes.data(), bytes.size());
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        bytes = bytes.subspan(static_cast<std::size_t>(n));
    }
    return true;
}

// Reads exactly bytes.size() bytes, waking early if the wake handle becomes readable.
bool readAll(PipeTransport::NativeHandle handle, PipeTransport::NativeHandle wake, std::span<std::byte> bytes) noexcept
{
    while (!bytes.empty())
    {
        pollfd fds[2] = {{static_cast<int>(handle), POLLIN, 0}, {static_cast<int>(wake), POLLIN, 0}};
        const int ready = ::poll(fds, 2, -1);
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0 || (fds[1].revents & (POLLIN | POLLHUP)) != 0) return false;
        const auto n = ::read(static_cast<int>(handle), bytes.data(), bytes.size());
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        bytes = bytes.subspan(static_cast<std::size_t>(n));
    }
    return true;
}

void closeHandle(PipeTransport::NativeHandle& handle) noexcept
{
    if (handle >= 0)
    {
        ::close(static_cast<int>(handle));
        handle = -1;
    }
}

// A write to a pipe whose reader has gone raises SIGPIPE, which terminates by
// default; keep a closed peer an error return instead.
void ignoreSigpipeIfDefault() noexcept
{
    struct sigaction current{};
    if (::sigaction(SIGPIPE, nullptr, &current) == 0 && current.sa_handler == SIG_DFL)
        ::signal(SIGPIPE, SIG_IGN);
}

#endif

} // namespace

PipeTransport::PipeTransport(NativeHandle readHandle, NativeHandle writeHandle) noexcept
    : readHandle_{readHandle}, writeHandle_{writeHandle}
{
#if !defined(_WIN32)
    ignoreSigpipeIfDefault();
    int wake[2];
    if (::pipe(wake) == 0)
    {
        wakeRead_ = wake[0];
        wakeWrite_ = wake[1];
    }
#endif
}

std::pair<std::unique_ptr<PipeTransport>, std::unique_ptr<PipeTransport>> PipeTransport::pair()
{
#if defined(_WIN32)
    HANDLE aToB[2] = {nullptr, nullptr};
    HANDLE bToA[2] = {nullptr, nullptr};
    // A large buffer lets a writer run ahead of the reader as a socket would.
    constexpr DWORD kBuffer = 1U << 20U;
    if (!CreatePipe(&aToB[0], &aToB[1], nullptr, kBuffer)) return {};
    if (!CreatePipe(&bToA[0], &bToA[1], nullptr, kBuffer))
    {
        CloseHandle(aToB[0]);
        CloseHandle(aToB[1]);
        return {};
    }
    auto native = [](HANDLE h) { return reinterpret_cast<NativeHandle>(h); };
    return {std::make_unique<PipeTransport>(native(bToA[0]), native(aToB[1])),
            std::make_unique<PipeTransport>(native(aToB[0]), native(bToA[1]))};
#else
    int aToB[2];
    int bToA[2];
    if (::pipe(aToB) != 0) return {};
    if (::pipe(bToA) != 0)
    {
        ::close(aToB[0]);
        ::close(aToB[1]);
        return {};
    }
    return {std::make_unique<PipeTransport>(bToA[0], aToB[1]), std::make_unique<PipeTransport>(aToB[0], bToA[1])};
#endif
}

PipeTransport::~PipeTransport()
{
    close();
    if (reader_.joinable())
    {
#if defined(_WIN32)
        // A blocking ReadFile is cancelled until the reader notices; it checks
        // nothing else, so retry in case the cancel lands between reads.
        while (WaitForSingleObject(reader_.native_handle(), 1) == WAIT_TIMEOUT)
            CancelSynchronousIo(reader_.native_handle());
#else
        const std::byte one{1};
        // Nothing to do if the wake write fails: the pipe is then already broken.
        if (wakeWrite_ >= 0 && ::write(static_cast<int>(wakeWrite_), &one, 1) < 0) {}
#endif
        reader_.join();
    }
    closeHandle(readHandle_);
#if !defined(_WIN32)
    closeHandle(wakeRead_);
    closeHandle(wakeWrite_);
#endif
}

void PipeTransport::start(IFrameSink& sink)
{
    reader_ = std::jthread{[this, &sink] { read(sink); }};
}

void PipeTransport::read(IFrameSink& sink)
{
    for (;;)
    {
        std::byte header[FrameReader::kHeaderBytes];
        if (!readAll(readHandle_, wakeRead_, header)) break;
        uint32_t body = 0U;
        for (std::size_t i = 0U; i < 4U; ++i) body |= static_cast<uint32_t>(std::to_integer<uint8_t>(header[i])) << (8U * i);
        if (body > kMaxBodyBytes)
        {
            close();
            sink.onClosed(TransportError::kProtocol);
            return;
        }
        // One buffer per frame, read into directly, so the sink can keep it.
        const std::size_t size = FrameReader::kHeaderBytes + body;
        std::shared_ptr<std::byte[]> frame{new std::byte[size]};
        std::memcpy(frame.get(), header, FrameReader::kHeaderBytes);
        if (!readAll(readHandle_, wakeRead_, {frame.get() + FrameReader::kHeaderBytes, body})) break;
        const std::span<const std::byte> bytes{frame.get(), size};
        sink.onFrameBuffer(std::shared_ptr<const void>{frame, frame.get()}, bytes);
    }
    // End of stream: make sure the peer sees ours too.
    close();
    sink.onClosed(TransportError::kClosed);
}

std::expected<void, TransportError> PipeTransport::send(std::span<const std::byte> frame)
{
    const std::span<const std::byte> parts[] = {frame};
    return sendParts(parts);
}

std::expected<void, TransportError> PipeTransport::sendParts(std::span<const std::span<const std::byte>> parts)
{
    std::lock_guard lock{writeMtx_};
    if (writeClosed_) return std::unexpected(TransportError::kClosed);
    for (auto part : parts)
    {
        if (!writeAll(writeHandle_, part)) return std::unexpected(TransportError::kClosed);
    }
    return {};
}

void PipeTransport::close() noexcept
{
    std::lock_guard lock{writeMtx_};
    if (writeClosed_) return;
    writeClosed_ = true;
    closeHandle(writeHandle_);
}

} // namespace sub0pipeline::remote
