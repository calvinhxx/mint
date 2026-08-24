add_executable(aiagent_tests aiagent_tests.cpp)
aiagent_configure_target(aiagent_tests)
target_link_libraries(aiagent_tests PRIVATE aiagent_core)

add_test(NAME aiagent_tests COMMAND aiagent_tests)
set_tests_properties(aiagent_tests PROPERTIES LABELS "unit;integration")

add_executable(aiagent_v1_2_tests v1_2/policy_and_recovery_tests.cpp)
aiagent_configure_target(aiagent_v1_2_tests)
target_link_libraries(aiagent_v1_2_tests PRIVATE aiagent_core)
add_test(NAME aiagent_v1_2_tests COMMAND aiagent_v1_2_tests)
set_tests_properties(aiagent_v1_2_tests PROPERTIES LABELS "v1.2;contract")

add_test(NAME aiagent_cli_version COMMAND $<TARGET_FILE:aiagent> --version)
set_tests_properties(
    aiagent_cli_version
    PROPERTIES
        LABELS "v1.2;smoke"
        PASS_REGULAR_EXPRESSION "aiagent 1\\.2\\.0"
)

add_test(
    NAME v1_fixture_lifecycle
    COMMAND ${CMAKE_COMMAND}
        -DFIXTURE_DIR=${PROJECT_SOURCE_DIR}/tests/fixtures/v1_broken_project
        -DWORK_DIR=${PROJECT_BINARY_DIR}/v1-fixture-lifecycle
        -DCTEST_COMMAND=${CMAKE_CTEST_COMMAND}
        -P ${PROJECT_SOURCE_DIR}/tests/verify_v1_fixture.cmake
)
set_tests_properties(v1_fixture_lifecycle PROPERTIES LABELS "acceptance")
