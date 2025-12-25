cmake_minimum_required(VERSION 3.20)

project(chlog VERSION 0.1.0 LANGUAGES CXX)

option(CHLOG_BUILD_EXAMPLES "Build chlog examples" ON)

add_library(chlog INTERFACE)
add_library(chlog::chlog ALIAS chlog)

target_include_directories(chlog
  INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_features(chlog INTERFACE cxx_std_20)

if (MSVC)
  target_compile_options(chlog INTERFACE /W4 /permissive-)
else()
  target_compile_options(chlog INTERFACE -Wall -Wextra -Wpedantic)
endif()

if (CHLOG_BUILD_EXAMPLES)
  include(${CMAKE_CURRENT_LIST_DIR}/examples/CMakeLists.txt)
endif()
