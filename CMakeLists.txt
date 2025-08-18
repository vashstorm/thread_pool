cmake_minimum_required(VERSION 3.10)
project(thread_pool VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

add_compile_options(-Wall -Wpedantic -Wconversion)

find_package(Threads REQUIRED)

add_library(thread_pool_headers INTERFACE)
target_include_directories(thread_pool_headers INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(example1 example1.cpp)
target_link_libraries(example1 PRIVATE thread_pool_headers Threads::Threads)
target_compile_features(example1 PRIVATE cxx_std_23)

add_executable(example2 example2.cpp)
target_link_libraries(example2 PRIVATE thread_pool_headers Threads::Threads)
target_compile_features(example2 PRIVATE cxx_std_23)