# examples/CMakeLists.txt
# Included from the top-level CMakeLists.txt via include(...)

add_executable(chlog_stress
  ${CMAKE_CURRENT_LIST_DIR}/chlog_stress.cpp
)
target_link_libraries(chlog_stress PRIVATE chlog::chlog)
set_target_properties(chlog_stress PROPERTIES OUTPUT_NAME "chlog_stress")

add_executable(chlog_single_thread_bench
  ${CMAKE_CURRENT_LIST_DIR}/single_thread_bench.cpp
)
target_link_libraries(chlog_single_thread_bench PRIVATE chlog::chlog)
set_target_properties(chlog_single_thread_bench PROPERTIES OUTPUT_NAME "chlog_single_thread_bench")
