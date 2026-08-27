find_package(GTest CONFIG REQUIRED)
include(GoogleTest)

add_executable(mint_unit_tests
    unit/console_tests.cpp
    unit/diagnostic_log_tests.cpp
    unit/runtime_settings_tests.cpp
)
mint_configure_target(mint_unit_tests)
target_include_directories(mint_unit_tests PRIVATE ${PROJECT_SOURCE_DIR}/src/infrastructure)
target_link_libraries(mint_unit_tests PRIVATE mint_cli GTest::gtest_main)
gtest_discover_tests(
    mint_unit_tests
    TEST_PREFIX "unit."
    PROPERTIES LABELS "unit"
)

add_executable(mint_tests mint_tests.cpp)
mint_configure_target(mint_tests)
target_include_directories(mint_tests PRIVATE ${PROJECT_SOURCE_DIR}/src/application)
target_link_libraries(mint_tests PRIVATE mint_core GTest::gtest)
gtest_discover_tests(
    mint_tests
    TEST_PREFIX "integration."
    PROPERTIES LABELS "integration"
)

add_executable(mint_v1_2_tests v1_2/policy_and_recovery_tests.cpp)
mint_configure_target(mint_v1_2_tests)
target_link_libraries(mint_v1_2_tests PRIVATE mint_core GTest::gtest)
gtest_discover_tests(
    mint_v1_2_tests
    TEST_PREFIX "contract.v1_2."
)

add_executable(mint_v1_3_tests v1_3/project_workflow_tests.cpp)
mint_configure_target(mint_v1_3_tests)
target_link_libraries(mint_v1_3_tests PRIVATE mint_core GTest::gtest_main)
gtest_discover_tests(
    mint_v1_3_tests
    TEST_PREFIX "contract.v1_3."
)

add_executable(mint_v1_4_tests v1_4/provider_contract_tests.cpp)
mint_configure_target(mint_v1_4_tests)
target_include_directories(mint_v1_4_tests PRIVATE ${PROJECT_SOURCE_DIR}/src/infrastructure)
target_link_libraries(mint_v1_4_tests PRIVATE mint_core GTest::gtest)
gtest_discover_tests(
    mint_v1_4_tests
    TEST_PREFIX "contract.v1_4."
    EXTRA_ARGS "--mint-executable=$<TARGET_FILE:mint>"
)

set_property(
    DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/DiscoveredTestLabels.cmake"
)

add_test(
    NAME architecture_console_boundary
    COMMAND ${CMAKE_COMMAND}
        -DSOURCE_DIR=${PROJECT_SOURCE_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyConsoleBoundary.cmake
)
set_tests_properties(architecture_console_boundary PROPERTIES LABELS "architecture;contract")

add_test(NAME mint_cli_version COMMAND $<TARGET_FILE:mint> --version)
set_tests_properties(
    mint_cli_version
    PROPERTIES
        LABELS "v1.4;smoke"
        PASS_REGULAR_EXPRESSION "mint 1\\.4\\.0"
)

add_test(NAME mint_cli_log_level COMMAND $<TARGET_FILE:mint> --log-level off --version)
set_tests_properties(
    mint_cli_log_level
    PROPERTIES
        LABELS "v1.4;smoke"
        PASS_REGULAR_EXPRESSION "mint 1\\.4\\.0"
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

add_test(
    NAME mint_v1_3_cli_workflow
    COMMAND ${CMAKE_COMMAND}
        -DMINT_EXECUTABLE=$<TARGET_FILE:mint>
        -DFIXTURE_DIR=${PROJECT_SOURCE_DIR}/tests/fixtures/v1_broken_project
        -DSTATE_DIR=${PROJECT_BINARY_DIR}/v1-3-cli-state
        -P ${PROJECT_SOURCE_DIR}/tests/verify_v1_3_cli.cmake
)
set_tests_properties(mint_v1_3_cli_workflow PROPERTIES LABELS "v1.3;acceptance")
