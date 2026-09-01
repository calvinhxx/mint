find_package(GTest CONFIG REQUIRED)
find_package(Python3 3.10 COMPONENTS Interpreter REQUIRED)
include(MintTesting)

add_library(mint_test_support STATIC
    support/command_helper.cpp
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
        unit/command_line_tests.cpp
        unit/diagnostic_log_tests.cpp
        unit/interaction_protocol_tests.cpp
        unit/path_tests.cpp
        unit/runtime_settings_tests.cpp
    LIBRARIES mint_cli
    INCLUDE_DIRECTORIES "${PROJECT_SOURCE_DIR}/src/infrastructure"
    TEST_PREFIX "unit."
    LABELS unit
)

mint_add_gtest(mint_tests
    SOURCES
        integration/agent_loop_tests.cpp
        integration/agent_token_budget_tests.cpp
        integration/change_transaction_tests.cpp
        integration/command_change_tracking_tests.cpp
        integration/command_runner_tests.cpp
        integration/model_client_tests.cpp
        integration/terminal_output_tests.cpp
        integration/tool_registry_tests.cpp
    LIBRARIES mint::core mint_cli mint_test_support
    INCLUDE_DIRECTORIES
        "${PROJECT_SOURCE_DIR}/src/application"
        "${PROJECT_SOURCE_DIR}/src/tools"
    TEST_PREFIX "integration."
    LABELS integration
)
target_compile_definitions(mint_tests PRIVATE
    MINT_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)

mint_add_gtest(mint_policy_recovery_tests
    SOURCES contract/policy_and_recovery_tests.cpp
    LIBRARIES mint::core
    TEST_PREFIX "contract.policy."
    LABELS contract policy recovery
)

mint_add_gtest(mint_project_workflow_tests
    GTEST_MAIN
    SOURCES contract/project_workflow_tests.cpp
    LIBRARIES mint::core
    TEST_PREFIX "contract.project."
    LABELS contract project
)

mint_add_gtest(mint_provider_protocol_tests
    GTEST_MAIN
    SOURCES contract/provider_protocol_tests.cpp
    LIBRARIES mint_cli mint_test_support
    INCLUDE_DIRECTORIES
        "${PROJECT_SOURCE_DIR}/src/cli"
        "${PROJECT_SOURCE_DIR}/src/infrastructure"
    TEST_PREFIX "contract.protocol."
    LABELS contract provider acceptance
)

mint_add_gtest(mint_provider_tests
    GTEST_MAIN
    SOURCES
        contract/model_usage_tests.cpp
        contract/provider_acceptance_tests.cpp
        contract/provider_catalog_tests.cpp
        contract/provider_profile_tests.cpp
    LIBRARIES mint_cli mint_test_support
    INCLUDE_DIRECTORIES
        "${PROJECT_SOURCE_DIR}/src/application"
        "${PROJECT_SOURCE_DIR}/src/cli"
        "${PROJECT_SOURCE_DIR}/src/infrastructure"
    TEST_PREFIX "contract.provider."
    LABELS contract provider acceptance
)
target_compile_definitions(mint_provider_tests PRIVATE
    MINT_PROVIDER_CONFIG_DIR="${PROJECT_SOURCE_DIR}/configs/providers"
)

add_test(
    NAME architecture_console_boundary
    COMMAND "${CMAKE_COMMAND}"
        -DSOURCE_DIR=${PROJECT_SOURCE_DIR}
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyConsoleBoundary.cmake"
)
set_tests_properties(architecture_console_boundary PROPERTIES LABELS "architecture;contract")

add_test(
    NAME architecture_application_boundary
    COMMAND "${CMAKE_COMMAND}"
        -DSOURCE_DIR=${PROJECT_SOURCE_DIR}
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyApplicationBoundary.cmake"
)
set_tests_properties(architecture_application_boundary PROPERTIES LABELS "architecture;contract")

add_test(
    NAME architecture_module_layout
    COMMAND "${CMAKE_COMMAND}"
        -DSOURCE_DIR=${PROJECT_SOURCE_DIR}
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyModuleLayout.cmake"
)
set_tests_properties(architecture_module_layout PROPERTIES LABELS "architecture;contract")

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
    NAME diagnostic_cli_contract
    COMMAND "${CMAKE_COMMAND}"
        -DMINT_EXECUTABLE=$<TARGET_FILE:mint>
        -DSOURCE_DIR=${PROJECT_SOURCE_DIR}
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyDiagnosticCli.cmake"
)
set_tests_properties(diagnostic_cli_contract PROPERTIES LABELS "smoke;contract")

add_test(
    NAME mint_provider_profile
    COMMAND $<TARGET_FILE:mint> provider
        --config "${PROJECT_SOURCE_DIR}/configs/providers/groq-chat.json"
        --json
)
set_tests_properties(
    mint_provider_profile
    PROPERTIES
        LABELS "provider;smoke"
        PASS_REGULAR_EXPRESSION "\"provider\":\"groq\""
)

add_test(
    NAME provider_regression_script_unit
    COMMAND "${Python3_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/tests/scripts/provider_regression_tests.py"
)
set_tests_properties(provider_regression_script_unit PROPERTIES LABELS "unit;provider")

add_test(
    NAME fixture_regression_script_unit
    COMMAND "${Python3_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/tests/scripts/fixture_regression_tests.py"
)
set_tests_properties(fixture_regression_script_unit PROPERTIES LABELS "unit;acceptance")

add_test(
    NAME eval_regression_script_unit
    COMMAND "${Python3_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/tests/scripts/eval_regression_tests.py"
)
set_tests_properties(eval_regression_script_unit PROPERTIES LABELS "unit;acceptance")

add_test(
    NAME release_evidence_script_unit
    COMMAND "${Python3_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/tests/scripts/release_evidence_tests.py"
)
set_tests_properties(release_evidence_script_unit PROPERTIES LABELS "unit;release")

add_test(
    NAME validate_version_script_unit
    COMMAND "${Python3_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/tests/scripts/validate_version_tests.py"
)
set_tests_properties(validate_version_script_unit PROPERTIES LABELS "unit;release")

add_test(
    NAME ci_workflow_script_unit
    COMMAND "${Python3_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/tests/scripts/ci_workflow_tests.py"
)
set_tests_properties(ci_workflow_script_unit PROPERTIES LABELS "unit;release")

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
        LABELS "provider;acceptance"
        PASS_REGULAR_EXPRESSION "\"operation\": \"provider_regression\""
)

add_test(
    NAME fixture_regression_offline
    COMMAND "${Python3_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/scripts/fixture-regression.py"
        --mint $<TARGET_FILE:mint>
        --config "${PROJECT_SOURCE_DIR}/configs/providers/openai-responses.json"
        --fixture "${PROJECT_SOURCE_DIR}/tests/fixtures/broken_cpp_project"
)
set_tests_properties(
    fixture_regression_offline
    PROPERTIES
        LABELS "provider;acceptance"
        PASS_REGULAR_EXPRESSION "\"operation\": \"fixture_regression\""
)

add_test(
    NAME broken_fixture_lifecycle
    COMMAND "${CMAKE_COMMAND}"
        -DFIXTURE_DIR=${PROJECT_SOURCE_DIR}/tests/fixtures/broken_cpp_project
        -DWORK_DIR=${PROJECT_BINARY_DIR}/broken-fixture-lifecycle
        -DCTEST_COMMAND=${CMAKE_CTEST_COMMAND}
        -P "${PROJECT_SOURCE_DIR}/tests/verify_fixture_lifecycle.cmake"
)
set_tests_properties(broken_fixture_lifecycle PROPERTIES LABELS acceptance)

add_test(
    NAME mint_project_cli_workflow
    COMMAND "${CMAKE_COMMAND}"
        -DMINT_EXECUTABLE=$<TARGET_FILE:mint>
        -DFIXTURE_DIR=${PROJECT_SOURCE_DIR}/tests/fixtures/broken_cpp_project
        -DSTATE_DIR=${PROJECT_BINARY_DIR}/project-cli-state
        -P "${PROJECT_SOURCE_DIR}/tests/verify_project_workflow.cmake"
)
set_tests_properties(mint_project_cli_workflow PROPERTIES LABELS "project;acceptance")
