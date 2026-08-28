find_package(GTest CONFIG REQUIRED)
include(MintTesting)

mint_add_gtest(mint_unit_tests
    GTEST_MAIN
    SOURCES
        unit/console_tests.cpp
        unit/diagnostic_log_tests.cpp
        unit/runtime_settings_tests.cpp
    LIBRARIES mint_cli
    INCLUDE_DIRECTORIES "${PROJECT_SOURCE_DIR}/src/infrastructure"
    TEST_PREFIX "unit."
    LABELS unit
)

mint_add_gtest(mint_tests
    SOURCES mint_tests.cpp
    LIBRARIES mint::core
    INCLUDE_DIRECTORIES "${PROJECT_SOURCE_DIR}/src/application"
    TEST_PREFIX "integration."
    LABELS integration
)
if(WIN32)
    target_link_libraries(mint_tests PRIVATE ws2_32)
endif()

mint_add_gtest(mint_v1_2_tests
    SOURCES v1_2/policy_and_recovery_tests.cpp
    LIBRARIES mint::core
    TEST_PREFIX "contract.v1_2."
    LABELS v1.2 contract
)

mint_add_gtest(mint_v1_3_tests
    GTEST_MAIN
    SOURCES v1_3/project_workflow_tests.cpp
    LIBRARIES mint::core
    TEST_PREFIX "contract.v1_3."
    LABELS v1.3 contract
)

mint_add_gtest(mint_v1_4_tests
    SOURCES v1_4/provider_contract_tests.cpp
    LIBRARIES mint::core
    INCLUDE_DIRECTORIES "${PROJECT_SOURCE_DIR}/src/infrastructure"
    TEST_PREFIX "contract.v1_4."
    LABELS v1.4 contract acceptance
    EXTRA_ARGS "--mint-executable=$<TARGET_FILE:mint>"
)

add_test(
    NAME architecture_console_boundary
    COMMAND "${CMAKE_COMMAND}"
        -DSOURCE_DIR=${PROJECT_SOURCE_DIR}
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyConsoleBoundary.cmake"
)
set_tests_properties(architecture_console_boundary PROPERTIES LABELS "architecture;contract")

add_test(NAME mint_cli_version COMMAND $<TARGET_FILE:mint> --version)
string(REPLACE "." "\\." mint_version_regex "${PROJECT_VERSION}")
set_tests_properties(
    mint_cli_version
    PROPERTIES
        LABELS "smoke"
        PASS_REGULAR_EXPRESSION "mint ${mint_version_regex}"
)

add_test(NAME mint_cli_log_level COMMAND $<TARGET_FILE:mint> --log-level off --version)
set_tests_properties(
    mint_cli_log_level
    PROPERTIES
        LABELS "smoke"
        PASS_REGULAR_EXPRESSION "mint ${mint_version_regex}"
)

add_test(
    NAME v1_fixture_lifecycle
    COMMAND "${CMAKE_COMMAND}"
        -DFIXTURE_DIR=${PROJECT_SOURCE_DIR}/tests/fixtures/v1_broken_project
        -DWORK_DIR=${PROJECT_BINARY_DIR}/v1-fixture-lifecycle
        -DCTEST_COMMAND=${CMAKE_CTEST_COMMAND}
        -P "${PROJECT_SOURCE_DIR}/tests/verify_v1_fixture.cmake"
)
set_tests_properties(v1_fixture_lifecycle PROPERTIES LABELS acceptance)

add_test(
    NAME mint_v1_3_cli_workflow
    COMMAND "${CMAKE_COMMAND}"
        -DMINT_EXECUTABLE=$<TARGET_FILE:mint>
        -DFIXTURE_DIR=${PROJECT_SOURCE_DIR}/tests/fixtures/v1_broken_project
        -DSTATE_DIR=${PROJECT_BINARY_DIR}/v1-3-cli-state
        -P "${PROJECT_SOURCE_DIR}/tests/verify_v1_3_cli.cmake"
)
set_tests_properties(mint_v1_3_cli_workflow PROPERTIES LABELS "v1.3;acceptance")
