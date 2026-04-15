#include "persistence.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace galahad {

namespace {

constexpr const char* kMagic = "GALAHAD-TP";

} // namespace

TemporalPersistence::TemporalPersistence(TemporalCore& core) : core_(core) {}

// v0 skeleton implementation.
//
// The current file writes only a format header:
//     GALAHAD-TP\n
//     version=0\n
//     payload=placeholder\n
//
// This exists so the plumbing (class shape, CMake target, load/save
// round-trip for an empty file) is wired before the real format lands.
// The next milestone replaces the payload with a full serialization of
// events, interning pools, and the refuted-branch set. Until then
// save() does not persist any event data and load() does not populate
// the core.
//
// The header is still validated on load so a corrupt or foreign file
// fails cleanly rather than silently succeeding.

void TemporalPersistence::save(const std::string& path) const {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) {
        throw std::runtime_error("TemporalPersistence::save: cannot open " + path);
    }
    f << kMagic << '\n'
      << "version=" << kFormatVersion << '\n'
      << "payload=placeholder\n";
    if (!f) {
        throw std::runtime_error("TemporalPersistence::save: write failed for " + path);
    }
}

void TemporalPersistence::load(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        throw std::runtime_error("TemporalPersistence::load: cannot open " + path);
    }

    std::string magic;
    std::getline(f, magic);
    if (magic != kMagic) {
        throw std::runtime_error(
            "TemporalPersistence::load: bad magic in " + path +
            " (got '" + magic + "')");
    }

    std::string version_line;
    std::getline(f, version_line);
    const std::string prefix = "version=";
    if (version_line.rfind(prefix, 0) != 0) {
        throw std::runtime_error(
            "TemporalPersistence::load: missing version line in " + path);
    }
    int version = 0;
    try {
        version = std::stoi(version_line.substr(prefix.size()));
    } catch (const std::exception&) {
        throw std::runtime_error(
            "TemporalPersistence::load: malformed version in " + path);
    }
    if (version != kFormatVersion) {
        throw std::runtime_error(
            "TemporalPersistence::load: unsupported format version " +
            std::to_string(version) + " in " + path);
    }

    // Payload handling lives with the next milestone. For v0 the loader
    // accepts a well-formed header and leaves the core untouched; callers
    // should treat this as "restart from empty" until the real format
    // lands. We deliberately do not silently fabricate events.
    (void)core_;
}

} // namespace galahad
