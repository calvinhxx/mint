find_package(GTest CONFIG REQUIRED)
find_package(Python3 3.10 COMPONENTS Interpreter REQUIRED)
include(MintTesting)

add_library(mint_test_support STATIC
    support/scripted_http_server.cpp
)
mint_configure_target(mint_test_support)
target_include_directories(mint_test_support PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/support"
)
if(WIN32)
    target_link_libraries(mint_test_support PUBLIC ws2_32)
endif()

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
    SOURCES
        mint_tests.cpp
        integration/change_transaction_tests.cpp
    LIBRARIES mint::core mint_test_support
    INCLUDE_DIRECTORIES "${PROJECT_SOURCE_DIR}/src/application"
    TEST_PREFIX "integration."
    LABELS integration
)

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
    GTEST_MAIN
    SOURCES v1_4/provider_contract_tests.cpp
    LIBRARIES mint_cli mint_test_support
    INCLUDE_DIRECTORIES
        "${PROJECT_SOURCE_DIR}/src/cli"
        "${PROJECT_SOURCE_DIR}/src/infrastructure"
    TEST_PREFIX "contract.v1_4."
    LABELS v1.4 contract acceptance
)

mint_add_gtest(mint_v1_5_tests
    GTEST_MAIN
    SOURCES
        v1_5/provider_acceptance_tests.cpp
        v1_5/provider_profile_tests.cpp
    LIBRARIES mint_cli
    INCLUDE_DIRECTORIES
        "${PROJECT_SOURCE_DIR}/src/cli"
        "${PROJECT_SOURCE_DIR}/src/infrastructure"
    TEST_PREFIX "contract.v1_5."
    LABELS v1.5 contract acceptance
)
target_compile_definitions(mint_v1_5_tests PRIVATE
    MINT_PROVIDER_CONFIG_DIR="${PROJECT_SOURCE_DIR}/configs/providers"
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
    NAME mint_provider_profile
    COMMAND $<TARGET_FILE:mint> provider
        --config "${PROJECT_SOURCE_DIR}/configs/providers/groq-chat.json"
        --json
)
set_tests_properties(
    mint_provider_profile
    PROPERTIES
        LABELS "v1.5;smoke"
        PASS_REGULAR_EXPRESSION "\"provider\":\"groq\""
)

add_test(
    NAME provider_regression_script_unit
    COMMAND "${Python3_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/tests/scripts/provider_regression_tests.py"
)
set_tests_properties(provider_regression_script_unit PROPERTIES LABELS "unit;v1.5")

add_test(
    NAME fixture_regression_script_unit
    COMMAND "${Python3_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/tests/scripts/fixture_regression_tests.py"
)
set_tests_properties(fixture_regression_script_unit PROPERTIES LABELS "unit;v1.5")

add_test(
    NAME release_evidence_script_unit
    COMMAND "${Python3_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/tests/scripts/release_evidence_tests.py"
)
set_tests_properties(release_evidence_script_unit PROPERTIES LABELS "unit;v1.5")

add_test(
    NAME provider_regression_offline
    COMMAND "${Python3_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/scripts/provider-regression.py"
        --mint $<TARGET_FILE:mint>
        --manifest "${PROJECT_SOURCE_DIR}/configs/provider-regression.json"
)
set_tests_properties(
    provider_regression_offline
    PROPERTIES
        LABELS "v1.5;acceptance"
        PASS_REGULAR_EXPRESSION "\"operation\": \"provider_regression\""
)

add_test(
    NAME fixture_regression_offline
    COMMAND "${Python3_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/scripts/fixture-regression.py"
        --mint $<TARGET_FILE:mint>
        --config "${PROJECT_SOURCE_DIR}/configs/providers/openai-responses.json"
        --fixture "${PROJECT_SOURCE_DIR}/tests/fixtures/v1_broken_project"
)
set_tests_properties(
    fixture_regression_offline
    PROPERTIES
        LABELS "v1.5;acceptance"
        PASS_REGULAR_EXPRESSION "\"operation\": \"fixture_regression\""
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
