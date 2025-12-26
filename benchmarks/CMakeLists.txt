add_executable(chlog_bench_loggers
  ${CMAKE_CURRENT_LIST_DIR}/loggers_bench.cpp
)

target_link_libraries(chlog_bench_loggers PRIVATE chlog::chlog)

# spdlog is optional (but recommended for comparisons)
find_package(spdlog CONFIG QUIET)
if (spdlog_FOUND)
  target_link_libraries(chlog_bench_loggers PRIVATE spdlog::spdlog)
  target_compile_definitions(chlog_bench_loggers PRIVATE CHLOG_HAS_SPDLOG=1)
  # Use fmt backend for chlog in this benchmark so formatting costs are comparable.
  target_compile_definitions(chlog_bench_loggers PRIVATE CHLOG_USE_FMT=1)
else()
  message(STATUS "spdlog not found; chlog_bench_loggers will run chlog-only")
endif()

if (MSVC)
  target_compile_options(chlog_bench_loggers PRIVATE /W4 /permissive-)
else()
  target_compile_options(chlog_bench_loggers PRIVATE -Wall -Wextra -Wpedantic)
endif()
