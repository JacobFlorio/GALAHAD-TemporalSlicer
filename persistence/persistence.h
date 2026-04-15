#pragma once
#include "../core/temporal_core.h"
#include <string>

namespace galahad {

// TemporalPersistence is the on-disk layer for TemporalCore.
//
// v1 format: a single binary file containing a header (magic + version)
// followed by the full event list in insertion order and the set of
// refuted branches. See persistence.cpp for the exact byte layout.
//
// Usage:
//
//   TemporalCore core;
//   // ... populate ...
//   TemporalPersistence(core).save("state.gtp");
//
//   // ... restart later ...
//   TemporalCore restored;
//   TemporalPersistence(restored).load("state.gtp");
//
// `load` is intended for a freshly constructed core; it does not clear
// existing state before reading, it only appends. Loading into a
// populated core produces a merge, not a restore.
//
// Endianness: v1 writes in host byte order. v0 of GALAHAD targets
// little-endian platforms (x86_64, ARM64). Cross-endian support is not
// a goal for this milestone.
class TemporalPersistence {
public:
    explicit TemporalPersistence(TemporalCore& core);

    // Serialize the full core state to `path`. Throws std::runtime_error
    // on I/O failure.
    void save(const std::string& path) const;

    // Load state from `path` into the core. Throws std::runtime_error on
    // missing file, bad magic, version mismatch, or truncated payload.
    void load(const std::string& path);

    // Format version written by save() and checked by load(). Bumped
    // whenever the on-disk layout changes in a non-compatible way.
    static constexpr std::uint32_t kFormatVersion = 1;

private:
    TemporalCore& core_;
};

} // namespace galahad
