#include "persistence.h"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace galahad {

// ----------------------------------------------------------------------------
// File layout (v1)
// ----------------------------------------------------------------------------
//
//   Header:
//     magic          : 10 bytes, literal "GALAHAD-TP" (no null terminator)
//     version        : uint32, must equal kFormatVersion
//
//   Body:
//     event_count    : uint32
//     events[event_count]:
//       id           : string (u32 length + bytes)
//       valid_from   : int64 nanoseconds since epoch
//       valid_to     : int64 nanoseconds since epoch
//       recorded_at  : int64 nanoseconds since epoch
//       type         : string
//       data_count   : uint32
//       data[...]    : (key string, value string) pairs
//       link_count   : uint32
//       links[...]   : string
//       branch_id    : string
//       confidence   : double (host IEEE-754)
//
//     refuted_count  : uint32
//     refuted[...]   : string
//
// All multi-byte fields are host byte order. v0 of GALAHAD targets little-
// endian platforms; cross-endian is not a v1 goal. A sanity cap of 256 MiB
// per string prevents runaway allocations on corrupt files.
//
// Events are written in deque insertion order, which is the order the core
// received them. On load we replay them in the same order via addEvent(),
// so causal_links that reference earlier events resolve naturally during
// index rebuild, and every event's recorded_at bumps the core's monotonic
// clock so subsequent now() calls never regress.
//
// Refuted branches are written last so the loader can register them after
// all events are in place. Order within the refuted set does not matter.
// ----------------------------------------------------------------------------

namespace {

constexpr const char* kMagic = "GALAHAD-TP";
constexpr std::size_t kMagicLen = 10;
constexpr std::uint32_t kMaxStringBytes = 256u * 1024u * 1024u;  // 256 MiB

void writeBytes(std::ostream& o, const void* p, std::size_t n) {
    o.write(static_cast<const char*>(p),
            static_cast<std::streamsize>(n));
    if (!o) throw std::runtime_error("persistence: write failed");
}

void readBytes(std::istream& i, void* p, std::size_t n) {
    i.read(static_cast<char*>(p), static_cast<std::streamsize>(n));
    if (!i) throw std::runtime_error("persistence: read failed (truncated)");
}

void writeU32(std::ostream& o, std::uint32_t v) {
    writeBytes(o, &v, sizeof(v));
}

std::uint32_t readU32(std::istream& i) {
    std::uint32_t v = 0;
    readBytes(i, &v, sizeof(v));
    return v;
}

void writeI64(std::ostream& o, std::int64_t v) {
    writeBytes(o, &v, sizeof(v));
}

std::int64_t readI64(std::istream& i) {
    std::int64_t v = 0;
    readBytes(i, &v, sizeof(v));
    return v;
}

void writeDouble(std::ostream& o, double v) {
    writeBytes(o, &v, sizeof(v));
}

double readDouble(std::istream& i) {
    double v = 0.0;
    readBytes(i, &v, sizeof(v));
    return v;
}

void writeString(std::ostream& o, const std::string& s) {
    if (s.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("persistence: string too long to serialize");
    }
    writeU32(o, static_cast<std::uint32_t>(s.size()));
    if (!s.empty()) writeBytes(o, s.data(), s.size());
}

std::string readString(std::istream& i) {
    std::uint32_t n = readU32(i);
    if (n > kMaxStringBytes) {
        throw std::runtime_error(
            "persistence: absurdly long string (" + std::to_string(n) +
            " bytes) — file likely corrupt");
    }
    std::string s(n, '\0');
    if (n > 0) readBytes(i, s.data(), n);
    return s;
}

std::int64_t tpToNanos(TimePoint tp) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               tp.time_since_epoch())
        .count();
}

TimePoint tpFromNanos(std::int64_t ns) {
    return TimePoint(std::chrono::nanoseconds(ns));
}

void writeEvent(std::ostream& o, const TemporalEvent& e) {
    writeString(o, e.id);
    writeI64(o, tpToNanos(e.valid_from));
    writeI64(o, tpToNanos(e.valid_to));
    writeI64(o, tpToNanos(e.recorded_at));
    writeString(o, e.type);
    writeU32(o, static_cast<std::uint32_t>(e.data.size()));
    for (const auto& [k, v] : e.data) {
        writeString(o, k);
        writeString(o, v);
    }
    writeU32(o, static_cast<std::uint32_t>(e.causal_links.size()));
    for (const auto& link : e.causal_links) writeString(o, link);
    writeString(o, e.branch_id);
    writeDouble(o, e.confidence);
}

TemporalEvent readEvent(std::istream& i) {
    TemporalEvent e;
    e.id = readString(i);
    e.valid_from = tpFromNanos(readI64(i));
    e.valid_to = tpFromNanos(readI64(i));
    e.recorded_at = tpFromNanos(readI64(i));
    e.type = readString(i);
    const std::uint32_t data_n = readU32(i);
    for (std::uint32_t j = 0; j < data_n; ++j) {
        auto k = readString(i);
        auto v = readString(i);
        e.data.emplace(std::move(k), std::move(v));
    }
    const std::uint32_t links_n = readU32(i);
    e.causal_links.reserve(links_n);
    for (std::uint32_t j = 0; j < links_n; ++j) {
        e.causal_links.push_back(readString(i));
    }
    e.branch_id = readString(i);
    e.confidence = readDouble(i);
    return e;
}

} // namespace

TemporalPersistence::TemporalPersistence(TemporalCore& core) : core_(core) {}

void TemporalPersistence::save(const std::string& path) const {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) {
        throw std::runtime_error(
            "TemporalPersistence::save: cannot open " + path);
    }

    writeBytes(f, kMagic, kMagicLen);
    writeU32(f, kFormatVersion);

    const auto events = core_.getAllEvents();
    writeU32(f, static_cast<std::uint32_t>(events.size()));
    for (const auto& e : events) writeEvent(f, e);

    const auto refuted = core_.getRefutedBranches();
    writeU32(f, static_cast<std::uint32_t>(refuted.size()));
    for (const auto& b : refuted) writeString(f, b);

    f.flush();
    if (!f) {
        throw std::runtime_error(
            "TemporalPersistence::save: write failed for " + path);
    }
}

void TemporalPersistence::load(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        throw std::runtime_error(
            "TemporalPersistence::load: cannot open " + path);
    }

    char magic[kMagicLen];
    readBytes(f, magic, kMagicLen);
    if (std::memcmp(magic, kMagic, kMagicLen) != 0) {
        throw std::runtime_error(
            "TemporalPersistence::load: bad magic in " + path);
    }

    const std::uint32_t version = readU32(f);
    if (version != kFormatVersion) {
        throw std::runtime_error(
            "TemporalPersistence::load: unsupported format version " +
            std::to_string(version) + " in " + path);
    }

    const std::uint32_t event_count = readU32(f);
    for (std::uint32_t i = 0; i < event_count; ++i) {
        core_.addEvent(readEvent(f));
    }

    const std::uint32_t refuted_count = readU32(f);
    for (std::uint32_t i = 0; i < refuted_count; ++i) {
        core_.refuteBranch(readString(f));
    }
}

} // namespace galahad
