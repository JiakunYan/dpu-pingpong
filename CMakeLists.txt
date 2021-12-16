cmake_minimum_required(VERSION 3.17)
project(dpdk-pingpong)

set(CMAKE_CXX_STANDARD 14)

find_package(PkgConfig REQUIRED)
pkg_check_modules(dpdk REQUIRED IMPORTED_TARGET libdpdk)
find_package(Threads REQUIRED)

add_executable(dpdk_pingpong dpdk_pingpong.c)
target_link_libraries(dpdk_pingpong PRIVATE PkgConfig::dpdk)

add_executable(dpu_fwd dpu_fwd.c)
target_link_libraries(dpu_fwd PRIVATE PkgConfig::dpdk Threads::Threads)
