cmake_minimum_required(VERSION 3.20)
project(GALAHAD_TemporalSlicer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Default to Release if the user didn't explicitly pick a build type.
# Debug builds still work with `cmake -DCMAKE_BUILD_TYPE=Debug`.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

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

add_library(temporal_persistence
    persistence/persistence.cpp
)
target_include_directories(temporal_persistence PUBLIC persistence)
target_link_libraries(temporal_persistence PUBLIC temporal_core)

# ---- tests ----
add_executable(test_temporal tests/test_temporal.cpp)
target_link_libraries(test_temporal PRIVATE temporal_core)

add_executable(test_engine tests/test_engine.cpp)
target_link_libraries(test_engine PRIVATE temporal_engine)

add_executable(test_adapter tests/test_adapter.cpp)
target_link_libraries(test_adapter PRIVATE temporal_adapter)

add_executable(test_persistence tests/test_persistence.cpp)
target_link_libraries(test_persistence PRIVATE temporal_persistence temporal_engine)

# ---- bench ----
add_executable(bench_temporal bench/main.cpp)
target_link_libraries(bench_temporal
    PRIVATE temporal_core temporal_engine temporal_persistence
)
