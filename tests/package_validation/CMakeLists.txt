cmake_minimum_required(VERSION 3.24)

project(MemoryPoolPackageValidation LANGUAGES CXX)

find_package(MemoryPool CONFIG REQUIRED)

add_executable(package_validation main.cpp)
target_compile_features(package_validation PRIVATE cxx_std_20)
target_link_libraries(package_validation PRIVATE MemoryPool::memory_pool)
