cmake_minimum_required(VERSION 3.24)

project(
    MemoryPool
    VERSION 0.1.0
    DESCRIPTION "A compact C++20 fixed-block memory pool library"
    LANGUAGES CXX
)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(MEMORYPOOL_BUILD_BENCHMARK "Build the standalone Release benchmark when test.cpp is present" ${PROJECT_IS_TOP_LEVEL})

add_library(memory_pool STATIC
    source/memory_pool/fixed_block_pool.cpp
    source/memory_pool/pool_memory_resource.cpp
    source/memory_pool/segregated_pool_resource.cpp
    source/memory_pool/version.cpp
)
add_library(MemoryPool::memory_pool ALIAS memory_pool)

target_compile_features(memory_pool PUBLIC cxx_std_20)
target_include_directories(memory_pool
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_compile_definitions(memory_pool
    PUBLIC
        MEMORY_POOL_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
        MEMORY_POOL_VERSION_MINOR=${PROJECT_VERSION_MINOR}
        MEMORY_POOL_VERSION_PATCH=${PROJECT_VERSION_PATCH}
        MEMORY_POOL_VERSION_STRING="${PROJECT_VERSION}"
)

if(MSVC)
    target_compile_options(memory_pool PRIVATE /W4 /permissive-)
else()
    target_compile_options(memory_pool PRIVATE -Wall -Wextra -Wpedantic)
endif()
target_compile_definitions(memory_pool PRIVATE $<$<CONFIG:Release>:NDEBUG>)
target_compile_options(memory_pool PRIVATE
    $<$<AND:$<CONFIG:Release>,$<CXX_COMPILER_ID:MSVC>>:/O2>
    $<$<AND:$<CONFIG:Release>,$<NOT:$<CXX_COMPILER_ID:MSVC>>>:-O3>
)

if(PROJECT_IS_TOP_LEVEL AND MEMORYPOOL_BUILD_BENCHMARK AND EXISTS "${PROJECT_SOURCE_DIR}/test.cpp")
    if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE STREQUAL "Release")
        message(FATAL_ERROR "MemoryPool benchmark must be configured with -DCMAKE_BUILD_TYPE=Release. Use -DMEMORYPOOL_BUILD_BENCHMARK=OFF for non-Release library-only builds.")
    endif()

    file(TO_CMAKE_PATH "${PROJECT_SOURCE_DIR}/results" MEMORYPOOL_RESULTS_DIR)
    add_executable(memory_pool_benchmark test.cpp)
    target_link_libraries(memory_pool_benchmark PRIVATE MemoryPool::memory_pool)
    target_compile_features(memory_pool_benchmark PRIVATE cxx_std_20)
    target_compile_definitions(memory_pool_benchmark PRIVATE MEMORY_POOL_RESULTS_DIR="${MEMORYPOOL_RESULTS_DIR}")
    if(MSVC)
        target_compile_options(memory_pool_benchmark PRIVATE /W4 /permissive-)
    else()
        target_compile_options(memory_pool_benchmark PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    target_compile_definitions(memory_pool_benchmark PRIVATE $<$<CONFIG:Release>:NDEBUG>)
    target_compile_options(memory_pool_benchmark PRIVATE
        $<$<AND:$<CONFIG:Release>,$<CXX_COMPILER_ID:MSVC>>:/O2>
        $<$<AND:$<CONFIG:Release>,$<NOT:$<CXX_COMPILER_ID:MSVC>>>:-O3>
    )
endif()

install(TARGETS memory_pool
    EXPORT MemoryPoolTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

configure_package_config_file(
    cmake/MemoryPoolConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/MemoryPoolConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/MemoryPool
)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/MemoryPoolConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/MemoryPoolConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/MemoryPoolConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/MemoryPool
)
install(EXPORT MemoryPoolTargets
    NAMESPACE MemoryPool::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/MemoryPool
)
