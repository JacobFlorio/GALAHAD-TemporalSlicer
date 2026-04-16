cmake_minimum_required(VERSION 3.20)
project(GALAHAD_TemporalSlicer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Suppress MSVC deprecation warnings for standard C functions (snprintf,
# strftime, etc.) that are perfectly safe in our usage.
if(MSVC)
    add_definitions(-D_CRT_SECURE_NO_WARNINGS)
endif()

# Default to Release if the user didn't explicitly pick a build type.
# Debug builds still work with `cmake -DCMAKE_BUILD_TYPE=Debug`.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# The Python bindings compile into a shared module that links our static
# libraries. Make sure the static libs are position-independent so the
# shared-object link succeeds on Linux.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(GALAHAD_BUILD_PYTHON "Build the Python bindings" ON)
option(GALAHAD_BUILD_TESTS  "Build the C++ test binaries"  ON)
option(GALAHAD_BUILD_BENCH  "Build the benchmark harness"  ON)

# ---- third-party: nlohmann_json (fetched) ----
include(FetchContent)
set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install OFF CACHE INTERNAL "")
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

# ---- libraries ----
add_library(temporal_core
    core/temporal_core.cpp
)
target_include_directories(temporal_core PUBLIC core)

add_library(temporal_engine
    engine/temporal_engine.cpp
)
target_include_directories(temporal_engine PUBLIC engine)
target_link_libraries(temporal_engine PUBLIC temporal_core)

add_library(temporal_adapter
    adapters/llm_tool_adapter.cpp
)
target_include_directories(temporal_adapter PUBLIC adapters)
target_link_libraries(temporal_adapter
    PUBLIC temporal_core temporal_engine nlohmann_json::nlohmann_json
)

add_library(temporal_anomaly
    anomaly/anomaly_detector.cpp
)
target_include_directories(temporal_anomaly PUBLIC anomaly)
target_link_libraries(temporal_anomaly PUBLIC temporal_core)

add_library(temporal_persistence
    persistence/persistence.cpp
)
target_include_directories(temporal_persistence PUBLIC persistence)
target_link_libraries(temporal_persistence PUBLIC temporal_core)

# ---- tests ----
if(GALAHAD_BUILD_TESTS)
    enable_testing()

    add_executable(test_temporal tests/test_temporal.cpp)
    target_link_libraries(test_temporal PRIVATE temporal_core)
    add_test(NAME test_temporal COMMAND test_temporal)

    add_executable(test_engine tests/test_engine.cpp)
    target_link_libraries(test_engine PRIVATE temporal_engine)
    add_test(NAME test_engine COMMAND test_engine)

    add_executable(test_adapter tests/test_adapter.cpp)
    target_link_libraries(test_adapter PRIVATE temporal_adapter)
    add_test(NAME test_adapter COMMAND test_adapter)

    add_executable(test_persistence tests/test_persistence.cpp)
    target_link_libraries(test_persistence PRIVATE temporal_persistence temporal_engine)
    add_test(NAME test_persistence COMMAND test_persistence)

    add_executable(test_anomaly tests/test_anomaly.cpp)
    target_link_libraries(test_anomaly PRIVATE temporal_anomaly)
    add_test(NAME test_anomaly COMMAND test_anomaly)
endif()

# ---- bench ----
if(GALAHAD_BUILD_BENCH)
    add_executable(bench_temporal bench/main.cpp)
    target_link_libraries(bench_temporal
        PRIVATE temporal_core temporal_engine temporal_persistence
    )
endif()

# ---- Python bindings ----
if(GALAHAD_BUILD_PYTHON)
    # Use pybind11's modern FindPython so it respects Python3_EXECUTABLE and
    # does not grab a Python whose dev headers are not installed. Without
    # this, pybind11's legacy finder can pick python3.11 by name even when
    # only python3.10 has usable headers.
    set(PYBIND11_FINDPYTHON ON)
    if(NOT DEFINED Python3_EXECUTABLE)
        find_program(Python3_EXECUTABLE NAMES python3.10 python3)
    endif()

    find_package(pybind11 QUIET)
    if(NOT pybind11_FOUND)
        FetchContent_Declare(
            pybind11
            GIT_REPOSITORY https://github.com/pybind/pybind11.git
            GIT_TAG v2.13.6
            GIT_SHALLOW TRUE
        )
        FetchContent_MakeAvailable(pybind11)
    endif()

    pybind11_add_module(galahad_py python/galahad.cpp)
    set_target_properties(galahad_py PROPERTIES
        OUTPUT_NAME "galahad"
        PREFIX ""
    )
    target_link_libraries(galahad_py
        PRIVATE
        temporal_core
        temporal_engine
        temporal_adapter
        temporal_anomaly
        temporal_persistence
        nlohmann_json::nlohmann_json
    )

    # Install the extension module into the wheel root so `import galahad`
    # resolves directly. scikit-build-core uses this install rule to
    # populate the wheel layout.
    install(TARGETS galahad_py
        LIBRARY DESTINATION .
        RUNTIME DESTINATION .
    )
endif()
