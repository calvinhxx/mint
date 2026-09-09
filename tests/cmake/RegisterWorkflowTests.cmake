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
