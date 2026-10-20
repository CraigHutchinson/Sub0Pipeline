// include/sub0pipeline/remote/pipe_transport.hpp
//
// PipeTransport -- ITransport over a pair of byte-stream handles, such as a
// child process's stdin and stdout or two anonymous pipes. Link
// Sub0Pipeline::Remote.
#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <utility>

#include "sub0pipeline/remote/transport.hpp"

namespace sub0pipeline::remote
{

/**
 * Frames over a byte stream: one handle to read from, one to write to.
 *
 * Writes each frame's parts with one write call per part, without
 * concatenating them. Reads each frame's header, then its body straight into
 * a buffer allocated for that frame, and hands the buffer to the sink with
 * ownership (IFrameSink::onFrameBuffer), so a payload that fits one frame is
 * never copied by the library.
 *
 * close() closes the write handle; the peer then reads end-of-stream and
 * closes its side, which ends this side's reads. Destruction also stops a
 * read that is still waiting.
 */
class PipeTransport final : public ITransport
{
public:
    /** A file descriptor on POSIX, a HANDLE on Windows. */
    using NativeHandle = std::intptr_t;

    /**
     * Takes ownership of two handles.
     * @param readHandle   The stream this side reads frames from.
     * @param writeHandle  The stream this side writes frames to.
     */
    PipeTransport(NativeHandle readHandle, NativeHandle writeHandle) noexcept;

    /**
     * Creates two connected ends over two anonymous pipes.
     * @return The ends, or nullptrs if the pipes could not be created.
     */
    [[nodiscard]] static std::pair<std::unique_ptr<PipeTransport>, std::unique_ptr<PipeTransport>> pair();

    /** Closes both handles and joins the reader thread. */
    ~PipeTransport() override;

    PipeTransport(const PipeTransport&)            = delete;
    PipeTransport& operator=(const PipeTransport&) = delete;

    void start(IFrameSink& sink) override;
    [[nodiscard]] std::expected<void, TransportError> send(std::span<const std::byte> frame) override;
    [[nodiscard]] std::expected<void, TransportError> sendParts(std::span<const std::span<const std::byte>> parts) override;
    void close() noexcept override;

private:
    void read(IFrameSink& sink);

    NativeHandle readHandle_;
    NativeHandle writeHandle_;
    NativeHandle wakeRead_{-1};  ///< POSIX: wakes a waiting read on destruction.
    NativeHandle wakeWrite_{-1};
    std::mutex   writeMtx_;      ///< Keeps frames from interleaving.
    bool         writeClosed_{false};
    std::jthread reader_;        // last: joined before the handles it uses are closed
};

} // namespace sub0pipeline::remote
