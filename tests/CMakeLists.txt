add_executable(common_tests unit/common_tests.cpp)
target_link_libraries(common_tests PRIVATE lsl_common)
lsl_set_warnings(common_tests)
lsl_copy_runtime(common_tests)
add_test(NAME unit/common COMMAND common_tests)
set_tests_properties(unit/common PROPERTIES TIMEOUT 60)

if(TARGET publisher AND TARGET player)
    add_test(NAME integration/cli
        COMMAND ${CMAKE_COMMAND}
            "-DPUBLISHER=$<TARGET_FILE:publisher>"
            "-DPLAYER=$<TARGET_FILE:player>"
            "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/integration/cli.cmake")
    set_tests_properties(integration/cli PROPERTIES TIMEOUT 60)
endif()
