#include "persistence.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

using namespace galahad;

int main() {
    namespace fs = std::filesystem;
    const fs::path tmp = fs::temp_directory_path() / "galahad_persistence_test.gtp";
    if (fs::exists(tmp)) fs::remove(tmp);

    // --- save produces a well-formed header file ---
    {
        TemporalCore core;
        TemporalPersistence pers(core);
        pers.save(tmp.string());

        std::ifstream f(tmp);
        assert(f.is_open());
        std::string line;
        std::getline(f, line);
        assert(line == "GALAHAD-TP");
        std::getline(f, line);
        assert(line == "version=0");
    }

    // --- load accepts a file we just wrote ---
    {
        TemporalCore core;
        TemporalPersistence pers(core);
        pers.load(tmp.string());  // should not throw
    }

    // --- load rejects a file with bad magic ---
    {
        const fs::path bad = fs::temp_directory_path() / "galahad_persistence_bad.gtp";
        {
            std::ofstream f(bad);
            f << "NOT-GALAHAD\nversion=0\npayload=placeholder\n";
        }
        TemporalCore core;
        TemporalPersistence pers(core);
        bool threw = false;
        try {
            pers.load(bad.string());
        } catch (const std::exception&) {
            threw = true;
        }
        assert(threw);
        fs::remove(bad);
    }

    // --- load rejects an unknown format version ---
    {
        const fs::path wrongver = fs::temp_directory_path() / "galahad_persistence_ver.gtp";
        {
            std::ofstream f(wrongver);
            f << "GALAHAD-TP\nversion=9999\npayload=placeholder\n";
        }
        TemporalCore core;
        TemporalPersistence pers(core);
        bool threw = false;
        try {
            pers.load(wrongver.string());
        } catch (const std::exception&) {
            threw = true;
        }
        assert(threw);
        fs::remove(wrongver);
    }

    // --- load on a missing file throws cleanly ---
    {
        TemporalCore core;
        TemporalPersistence pers(core);
        bool threw = false;
        try {
            pers.load("/this/path/does/not/exist.gtp");
        } catch (const std::exception&) {
            threw = true;
        }
        assert(threw);
    }

    fs::remove(tmp);
    std::cout << "test_persistence: OK (skeleton header round-trip, "
              << "real format in next milestone)\n";
    return 0;
}
