cmake_minimum_required(VERSION 3.20)

project(chlog VERSION 1.0.0 LANGUAGES CXX)

option(CHLOG_BUILD_EXAMPLES "Build chlog examples" ON)
option(CHLOG_BUILD_BENCHMARKS "Build chlog benchmarks" ON)
option(CHLOG_USE_FMT "Use fmt instead of std::format" OFF)
include(CTest)
option(CHLOG_BUILD_TESTS "Build chlog regression tests" ${BUILD_TESTING})

include(GNUInstallDirs)

add_library(chlog INTERFACE)
add_library(chlog::chlog ALIAS chlog)

target_include_directories(chlog
  INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_compile_features(chlog INTERFACE cxx_std_20)
find_package(Threads REQUIRED)
target_link_libraries(chlog INTERFACE Threads::Threads)
if (CHLOG_USE_FMT)
  find_package(fmt CONFIG REQUIRED)
  target_link_libraries(chlog INTERFACE fmt::fmt)
  target_compile_definitions(chlog INTERFACE CHLOG_USE_FMT=1)
endif()

if (MSVC)
  target_compile_options(chlog INTERFACE /W4 /permissive-)
else()
  target_compile_options(chlog INTERFACE -Wall -Wextra -Wpedantic)
endif()

if (CHLOG_BUILD_EXAMPLES)
  include(${CMAKE_CURRENT_LIST_DIR}/examples/CMakeLists.txt)
endif()

if (CHLOG_BUILD_BENCHMARKS)
  include(${CMAKE_CURRENT_LIST_DIR}/benchmarks/CMakeLists.txt)
endif()

if (CHLOG_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# --------------------------- Install / Package ---------------------------

include(CMakePackageConfigHelpers)

install(
  DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(
  TARGETS chlog
  EXPORT chlogTargets
)

install(
  EXPORT chlogTargets
  FILE chlogTargets.cmake
  NAMESPACE chlog::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/chlog
)

configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/chlogConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/chlogConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/chlog
)

write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/chlogConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMinorVersion
)

install(
  FILES
    ${CMAKE_CURRENT_BINARY_DIR}/chlogConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/chlogConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/chlog
)
