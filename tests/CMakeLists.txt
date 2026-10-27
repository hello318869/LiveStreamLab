add_executable(common_tests unit/common_tests.cpp)
target_link_libraries(common_tests PRIVATE lsl_common)
lsl_set_warnings(common_tests)
lsl_copy_runtime(common_tests)
add_test(NAME unit/common COMMAND common_tests)
set_tests_properties(unit/common PROPERTIES TIMEOUT 60)

add_executable(player_playback_tests unit/player_playback_tests.cpp)
target_link_libraries(player_playback_tests PRIVATE lsl_common lsl_sync)
lsl_set_warnings(player_playback_tests)
lsl_copy_runtime(player_playback_tests)
add_test(NAME unit/player_playback COMMAND player_playback_tests)
set_tests_properties(unit/player_playback PROPERTIES TIMEOUT 15)

if(WIN32)
    add_executable(desktop_capture_tests integration/desktop_capture_tests.cpp)
    target_link_libraries(desktop_capture_tests PRIVATE lsl_capture lsl_publisher_pipeline)
    lsl_set_warnings(desktop_capture_tests)
    lsl_copy_runtime(desktop_capture_tests)
    add_test(NAME integration/desktop_capture COMMAND desktop_capture_tests)
    set_tests_properties(integration/desktop_capture PROPERTIES TIMEOUT 20)
endif()

if(TARGET livestreamlab)
    add_test(NAME integration/cli
        COMMAND ${CMAKE_COMMAND}
            "-DLIVESTREAMLAB=$<TARGET_FILE:livestreamlab>"
            "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/integration/cli.cmake")
    set_tests_properties(integration/cli PROPERTIES TIMEOUT 60)
endif()
