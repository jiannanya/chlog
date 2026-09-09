add_executable(chlog_bench_loggers
  ${CMAKE_CURRENT_LIST_DIR}/loggers_bench.cpp
)

target_link_libraries(chlog_bench_loggers PRIVATE chlog::chlog)

# spdlog is optional (but recommended for comparisons)
find_package(spdlog CONFIG QUIET)
if (spdlog_FOUND)
  target_link_libraries(chlog_bench_loggers PRIVATE spdlog::spdlog)
  target_compile_definitions(chlog_bench_loggers PRIVATE CHLOG_HAS_SPDLOG=1)
  # Some spdlog packages use std::format or do not export fmt headers.
  find_package(fmt CONFIG QUIET)
  if (fmt_FOUND AND NOT CHLOG_USE_FMT)
    target_link_libraries(chlog_bench_loggers PRIVATE fmt::fmt)
    target_compile_definitions(chlog_bench_loggers PRIVATE CHLOG_USE_FMT=1)
  endif()
else()
  message(STATUS "spdlog not found; chlog_bench_loggers will run chlog-only")
endif()

add_executable(chlog_render_bench ${CMAKE_CURRENT_LIST_DIR}/render_bench.cpp)
target_link_libraries(chlog_render_bench PRIVATE chlog::chlog)

if (MSVC)
  target_compile_options(chlog_bench_loggers PRIVATE /W4 /permissive-)
else()
  target_compile_options(chlog_bench_loggers PRIVATE -Wall -Wextra -Wpedantic)
endif()
