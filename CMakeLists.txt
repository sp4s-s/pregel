cmake_minimum_required(VERSION 3.20)
project(PregelEngine VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -DNDEBUG")
set(CMAKE_CXX_FLAGS_DEBUG "-O0 -g -fsanitize=address,undefined")

find_package(Threads REQUIRED)

find_package(MPI QUIET)
if(MPI_CXX_FOUND)
    message(STATUS "MPI found: ${MPI_CXX_COMPILER}")
    add_definitions(-DUSE_MPI=1)
else()
    message(STATUS "MPI not found — using simulated multi-process mode")
    add_definitions(-DUSE_MPI=0)
endif()

add_compile_options(-Wall -Wextra -Wpedantic -Wno-unused-parameter)

include_directories(include)

set(ENGINE_SOURCES
    src/main.cpp
    src/graph_engine.cpp
    src/vertex.cpp
    src/message_bus.cpp
    src/worker_node.cpp
    src/coordinator.cpp
    src/heartbeat.cpp
    src/numa_scheduler.cpp
    src/order_book_graph.cpp
    src/metrics.cpp
    src/rdma_transport.cpp
)

add_executable(pregel_engine ${ENGINE_SOURCES})

target_link_libraries(pregel_engine
    PRIVATE
    Threads::Threads
)

if(MPI_CXX_FOUND)
    target_link_libraries(pregel_engine PRIVATE MPI::MPI_CXX)
    target_include_directories(pregel_engine PRIVATE ${MPI_CXX_INCLUDE_DIRS})
endif()

add_executable(bench_throughput
    src/bench_throughput.cpp
    src/graph_engine.cpp
    src/vertex.cpp
    src/message_bus.cpp
    src/worker_node.cpp
    src/coordinator.cpp
    src/heartbeat.cpp
    src/numa_scheduler.cpp
    src/order_book_graph.cpp
    src/metrics.cpp
    src/rdma_transport.cpp
)

target_link_libraries(bench_throughput PRIVATE Threads::Threads)
if(MPI_CXX_FOUND)
    target_link_libraries(bench_throughput PRIVATE MPI::MPI_CXX)
endif()

message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "C++ standard: ${CMAKE_CXX_STANDARD}")
