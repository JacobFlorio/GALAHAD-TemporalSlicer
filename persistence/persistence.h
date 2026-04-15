#pragma once
#include "../core/temporal_core.h"
#include <string>

namespace galahad {

// TemporalPersistence is the on-disk layer for TemporalCore.
//
// v0 skeleton: this header establishes the public API shape. The real
// format (append-only log + mmap sidecar for the interning pools and
// refuted-branch set, crash-safe replay on startup) lands in the next
// milestone.
//
// Usage:
//   TemporalCore core;
//   TemporalPersistence pers(core);
//   pers.save("state.gtp");
//   // ... restart later ...
//   TemporalCore restored;
//   TemporalPersistence(restored).load("state.gtp");
//
// `load` is intended for a freshly constructed core; it does not clear
// any existing state. `save` writes a complete snapshot.
class TemporalPersistence {
public:
    explicit TemporalPersistence(TemporalCore& core);

    // Serialize the full core state to `path`. Throws std::runtime_error
    // on I/O failure.
    void save(const std::string& path) const;

    // Load state from `path` into the core. Throws std::runtime_error on
    // missing file, parse error, or version mismatch.
    void load(const std::string& path);

    // Format version written by save() and checked by load(). Bumped
    // whenever the on-disk layout changes in a non-compatible way.
    static constexpr int kFormatVersion = 0;

private:
    TemporalCore& core_;
};

} // namespace galahad
