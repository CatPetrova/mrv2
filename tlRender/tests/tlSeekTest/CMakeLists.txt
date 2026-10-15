add_executable(tlSeekTest main.cpp)
target_link_libraries(tlSeekTest PRIVATE tlTimeline)
add_test(NAME tlSeekScheduling COMMAND tlSeekTest)
set_tests_properties(tlSeekScheduling PROPERTIES TIMEOUT 30)

add_executable(tlSeekBenchmark benchmark.cpp)
target_link_libraries(tlSeekBenchmark PRIVATE tlTimeline)
