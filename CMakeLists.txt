cmake_minimum_required(VERSION 3.20)
project(GALAHAD_TemporalSlicer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(temporal_core
    core/temporal_core.cpp
)
target_include_directories(temporal_core PUBLIC core)

add_library(temporal_engine
    engine/temporal_engine.cpp
)
target_include_directories(temporal_engine PUBLIC engine)
target_link_libraries(temporal_engine PUBLIC temporal_core)

add_executable(test_temporal tests/test_temporal.cpp)
target_link_libraries(test_temporal PRIVATE temporal_core)

add_executable(test_engine tests/test_engine.cpp)
target_link_libraries(test_engine PRIVATE temporal_engine)

add_executable(bench_temporal bench/main.cpp)
target_link_libraries(bench_temporal PRIVATE temporal_core temporal_engine)
