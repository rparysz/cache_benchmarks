cmake_minimum_required(VERSION 3.20)
project(cache_benchmarks CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_options(-O2 -march=native)

# Enable compile command to ease indexing with e.g. clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

# After configure, symlink compile_commands.json to project root so clangd finds
# it without needing a manual step. Works on Linux/macOS; no-op on Windows.
add_custom_target(
  symlink_compile_commands ALL
  COMMAND
    ${CMAKE_COMMAND} -E create_symlink ${CMAKE_BINARY_DIR}/compile_commands.json
    ${CMAKE_SOURCE_DIR}/compile_commands.json
  COMMENT "Symlinking compile_commands.json to project root")

# Google Benchmark. If configure fails here with "Could not find a package
# configuration file provided by benchmark", install the dev package:
#   Fedora: sudo dnf install google-benchmark-devel
#   Ubuntu: sudo apt install libbenchmark-dev
#   Arch:   sudo pacman -S benchmark
find_package(benchmark REQUIRED)
find_package(Threads REQUIRED)

add_executable(sequential_access src/sequential_access.cpp)
add_executable(strided_access src/strided_access.cpp)
add_executable(pointer_chasing src/pointer_chasing.cpp)
add_executable(aos_vs_soa src/aos_vs_soa.cpp)
add_executable(aos_vs_soa_novec src/aos_vs_soa.cpp)
target_compile_options(aos_vs_soa_novec PRIVATE -fno-tree-vectorize)
add_executable(false_sharing src/false_sharing.cpp)

add_executable(matrix_multiply src/matrix_multiply.cpp)

set(ALL_TARGETS
    sequential_access
    strided_access
    pointer_chasing
    aos_vs_soa
    aos_vs_soa_novec
    false_sharing
    matrix_multiply
)

foreach(target ${ALL_TARGETS})
  target_link_libraries(${target} benchmark::benchmark)
  target_include_directories(${target} PRIVATE ${CMAKE_SOURCE_DIR}/include)
endforeach()

target_link_libraries(false_sharing benchmark::benchmark Threads::Threads)
