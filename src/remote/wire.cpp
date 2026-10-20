// src/remote/wire.cpp
//
// Encoding and decoding of the remote protocol's frames.

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "sub0pipeline/remote/transport.hpp"
#include "sub0pipeline/remote/wire.hpp"

namespace sub0pipeline::remote::wire
{

namespace
{

// The Pipeline's own graph limit; a request beyond it could not run.
constexpr uint32_t kMaxJobs = 65536U;

class Writer
{
public:
    explicit Writer(FrameType type)
    {
        out_.resize(FrameReader::kHeaderBytes);
        out_[4] = static_cast<std::byte>(type);
    }

    void u8(uint8_t v) { out_.push_back(static_cast<std::byte>(v)); }
    void u16(uint16_t v) { little(v, 2U); }
    void u32(uint32_t v) { little(v, 4U); }
    void u64(uint64_t v) { little(v, 8U); }

    void string(std::string_view s)
    {
        u16(static_cast<uint16_t>(s.size()));
        for (char c : s) out_.push_back(static_cast<std::byte>(c));
    }

    void blob(std::span<const std::byte> b)
    {
        u32(static_cast<uint32_t>(b.size()));
        out_.insert(out_.end(), b.begin(), b.end());
    }

    void payload(const Payload& p)
    {
        u8(static_cast<uint8_t>(p.kind()));
        blob(p.bytes());
    }

    std::vector<std::byte> finish()
    {
        const auto body = static_cast<uint32_t>(out_.size() - FrameReader::kHeaderBytes);
        for (std::size_t i = 0U; i < 4U; ++i) out_[i] = static_cast<std::byte>((body >> (8U * i)) & 0xFFU);
        return std::move(out_);
    }

private:
    void little(uint64_t v, std::size_t n)
    {
        for (std::size_t i = 0U; i < n; ++i) out_.push_back(static_cast<std::byte>((v >> (8U * i)) & 0xFFU));
    }

    std::vector<std::byte> out_;
};

// Bounds-checked reads; any failure latches and every later read fails.
class Reader
{
public:
    explicit Reader(std::span<const std::byte> body) noexcept : body_{body} {}

    [[nodiscard]] bool ok() const noexcept { return ok_; }
    [[nodiscard]] std::size_t remaining() const noexcept { return ok_ ? body_.size() - pos_ : 0U; }

    uint8_t u8() noexcept { return static_cast<uint8_t>(little(1U)); }
    uint16_t u16() noexcept { return static_cast<uint16_t>(little(2U)); }
    uint32_t u32() noexcept { return static_cast<uint32_t>(little(4U)); }
    uint64_t u64() noexcept { return little(8U); }

    std::string string()
    {
        const auto n = u16();
        if (!take(n)) return {};
        std::string s(n, '\0');
        for (std::size_t i = 0U; i < n; ++i) s[i] = static_cast<char>(body_[pos_ - n + i]);
        return s;
    }

    std::vector<std::byte> blob()
    {
        const auto n = u32();
        if (!take(n)) return {};
        return {body_.begin() + static_cast<std::ptrdiff_t>(pos_ - n), body_.begin() + static_cast<std::ptrdiff_t>(pos_)};
    }

    std::optional<Payload> payload()
    {
        const auto kind = u8();
        auto data = blob();
        if (!ok_) return std::nullopt;
        switch (static_cast<PayloadKind>(kind))
        {
            case PayloadKind::kNone: return data.empty() ? std::optional<Payload>{Payload{}} : std::nullopt;
            case PayloadKind::kInline: return Payload::inlineBytes(std::move(data));
            case PayloadKind::kHandle:
                if (data.size() > kMaxHandleBytes) return std::nullopt;
                return Payload::handle(data);
        }
        return std::nullopt;
    }

private:
    bool take(std::size_t n) noexcept
    {
        if (!ok_ || body_.size() - pos_ < n)
        {
            ok_ = false;
            return false;
        }
        pos_ += n;
        return true;
    }

    uint64_t little(std::size_t n) noexcept
    {
        if (!take(n)) return 0U;
        uint64_t v = 0U;
        for (std::size_t i = 0U; i < n; ++i)
            v |= static_cast<uint64_t>(std::to_integer<uint8_t>(body_[pos_ - n + i])) << (8U * i);
        return v;
    }

    std::span<const std::byte> body_;
    std::size_t pos_{0U};
    bool ok_{true};
};

// Returns the body of a well-formed frame of @p type.
std::optional<std::span<const std::byte>> bodyOf(std::span<const std::byte> frame, FrameType type) noexcept
{
    if (frameType(frame) != type) return std::nullopt;
    return frame.subspan(FrameReader::kHeaderBytes);
}

bool fits(const Payload& p) noexcept
{
    if (p.kind() == PayloadKind::kHandle) return p.bytes().size() <= kMaxHandleBytes;
    return p.bytes().size() <= std::numeric_limits<uint32_t>::max();
}

} // namespace

std::vector<std::byte> encodeHello(FrameType type, const Hello& hello)
{
    Writer w{type};
    w.u32(kProtocolMagic);
    w.u16(hello.major);
    w.u16(hello.minor);
    w.u64(hello.fingerprint);
    w.u32(hello.maxInFlight);
    w.u32(hello.maxInlineBytes);
    return w.finish();
}

std::vector<std::byte> encodeReject(const HelloReject& reject)
{
    Writer w{FrameType::kHelloReject};
    w.u8(static_cast<uint8_t>(reject.reason));
    w.string(std::string_view{reject.detail}.substr(0U, UINT16_MAX));
    return w.finish();
}

std::optional<std::vector<std::byte>> encodeRequest(uint64_t id, const SubGraphRequest& request)
{
    if (request.jobs().size() > kMaxJobs) return std::nullopt;
    Writer w{FrameType::kRequest};
    w.u64(id);
    w.u32(static_cast<uint32_t>(request.jobs().size()));
    for (const auto& job : request.jobs())
    {
        if (job.kind.name.size() > UINT16_MAX || !fits(job.input)) return std::nullopt;
        w.string(job.kind.name);
        w.u32(job.kind.version);
        w.u8(job.exported ? 1U : 0U);
        w.payload(job.input);
    }
    w.u32(static_cast<uint32_t>(request.edges().size()));
    for (const auto& edge : request.edges())
    {
        w.u32(edge.from);
        w.u32(edge.to);
    }
    return w.finish();
}

std::vector<std::byte> encodeReply(uint64_t id, const SubGraphReply& reply)
{
    Writer w{FrameType::kReply};
    w.u64(id);
    w.u32(static_cast<uint32_t>(reply.results.size()));
    for (const auto& result : reply.results)
    {
        w.u8(static_cast<uint8_t>(result.status));
        w.u8(static_cast<uint8_t>(result.error));
        w.string(std::string_view{result.message}.substr(0U, UINT16_MAX));
        w.payload(result.output);
    }
    return w.finish();
}

std::vector<std::byte> encodeCancel(uint64_t id)
{
    Writer w{FrameType::kCancel};
    w.u64(id);
    return w.finish();
}

std::optional<FrameType> frameType(std::span<const std::byte> frame) noexcept
{
    if (frame.size() < FrameReader::kHeaderBytes) return std::nullopt;
    uint32_t body = 0U;
    for (std::size_t i = 0U; i < 4U; ++i) body |= static_cast<uint32_t>(std::to_integer<uint8_t>(frame[i])) << (8U * i);
    if (body != frame.size() - FrameReader::kHeaderBytes) return std::nullopt;
    const auto type = std::to_integer<uint8_t>(frame[4]);
    if (type < static_cast<uint8_t>(FrameType::kHello) || type > static_cast<uint8_t>(FrameType::kCancel))
        return std::nullopt;
    return static_cast<FrameType>(type);
}

std::optional<Hello> decodeHello(std::span<const std::byte> frame)
{
    auto type = frameType(frame);
    if (type != FrameType::kHello && type != FrameType::kHelloAck) return std::nullopt;
    Reader r{frame.subspan(FrameReader::kHeaderBytes)};
    if (r.u32() != kProtocolMagic) return std::nullopt;
    Hello hello;
    hello.major = r.u16();
    hello.minor = r.u16();
    hello.fingerprint = r.u64();
    hello.maxInFlight = r.u32();
    hello.maxInlineBytes = r.u32();
    // A newer minor version may append fields, which are skipped.
    if (!r.ok()) return std::nullopt;
    return hello;
}

std::optional<HelloReject> decodeReject(std::span<const std::byte> frame)
{
    auto body = bodyOf(frame, FrameType::kHelloReject);
    if (!body) return std::nullopt;
    Reader r{*body};
    HelloReject reject;
    reject.reason = static_cast<RejectReason>(r.u8());
    reject.detail = r.string();
    if (!r.ok()) return std::nullopt;
    return reject;
}

std::optional<std::pair<uint64_t, SubGraphRequest>> decodeRequest(std::span<const std::byte> frame)
{
    auto body = bodyOf(frame, FrameType::kRequest);
    if (!body) return std::nullopt;
    Reader r{*body};
    const auto id = r.u64();
    const auto jobs = r.u32();
    if (!r.ok() || jobs > kMaxJobs) return std::nullopt;
    SubGraphRequest request;
    for (uint32_t i = 0U; i < jobs; ++i)
    {
        JobKindId kind;
        kind.name = r.string();
        kind.version = r.u32();
        const bool exported = r.u8() != 0U;
        auto input = r.payload();
        if (!r.ok() || !input) return std::nullopt;
        request.add(std::move(kind), std::move(*input), exported);
    }
    const auto edges = r.u32();
    if (!r.ok() || edges > r.remaining() / 8U) return std::nullopt;
    for (uint32_t i = 0U; i < edges; ++i)
    {
        const auto from = r.u32();
        const auto to = r.u32();
        if (!r.ok() || !request.edge(from, to)) return std::nullopt;
    }
    if (!r.ok()) return std::nullopt;
    return std::pair{id, std::move(request)};
}

std::optional<std::pair<uint64_t, SubGraphReply>> decodeReply(std::span<const std::byte> frame)
{
    auto body = bodyOf(frame, FrameType::kReply);
    if (!body) return std::nullopt;
    Reader r{*body};
    const auto id = r.u64();
    const auto count = r.u32();
    if (!r.ok() || count > kMaxJobs) return std::nullopt;
    SubGraphReply reply;
    reply.results.reserve(count);
    for (uint32_t i = 0U; i < count; ++i)
    {
        JobResult result;
        result.status = static_cast<JobStatus>(r.u8());
        result.error = static_cast<PipelineError>(r.u8());
        result.message = r.string();
        auto output = r.payload();
        if (!r.ok() || !output) return std::nullopt;
        result.output = std::move(*output);
        reply.results.push_back(std::move(result));
    }
    return std::pair{id, std::move(reply)};
}

std::optional<uint64_t> decodeCancel(std::span<const std::byte> frame) noexcept
{
    auto body = bodyOf(frame, FrameType::kCancel);
    if (!body) return std::nullopt;
    Reader r{*body};
    const auto id = r.u64();
    if (!r.ok()) return std::nullopt;
    return id;
}

} // namespace sub0pipeline::remote::wire
