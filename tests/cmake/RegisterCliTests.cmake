add_test(
    NAME localization_catalog_contract
    COMMAND "${Python3_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/tests/scripts/localization_catalog_tests.py"
        "${PROJECT_SOURCE_DIR}"
)
set_tests_properties(localization_catalog_contract PROPERTIES LABELS "unit;contract;localization")

add_test(
    NAME comment_language_contract
    COMMAND "${Python3_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/tests/scripts/comment_language_tests.py"
        "${PROJECT_SOURCE_DIR}"
)
set_tests_properties(comment_language_contract PROPERTIES LABELS "contract;style")

add_test(NAME mint_cli_help_english COMMAND $<TARGET_FILE:mint> --lang en --help)
set_tests_properties(
    mint_cli_help_english
    PROPERTIES
        LABELS "smoke;localization"
        PASS_REGULAR_EXPRESSION "Lightweight General AI Agent"
)

add_test(NAME mint_cli_help_chinese COMMAND $<TARGET_FILE:mint> --lang zh-CN --help)
set_tests_properties(
    mint_cli_help_chinese
    PROPERTIES
        LABELS "smoke;localization"
        PASS_REGULAR_EXPRESSION "轻量通用 AI Agent"
)

add_test(NAME mint_cli_version COMMAND $<TARGET_FILE:mint> --version)
string(REPLACE "." "\\." mint_version_regex "${PROJECT_VERSION}")
set_tests_properties(
    mint_cli_version
    PROPERTIES
        LABELS "smoke"
        PASS_REGULAR_EXPRESSION "mint ${mint_version_regex}"
)

add_test(
    NAME diagnostic_cli_contract
    COMMAND "${CMAKE_COMMAND}"
        -DMINT_EXECUTABLE=$<TARGET_FILE:mint>
        -DSOURCE_DIR=${PROJECT_SOURCE_DIR}
        -P "${CMAKE_CURRENT_LIST_DIR}/VerifyDiagnosticCli.cmake"
)
set_tests_properties(diagnostic_cli_contract PROPERTIES LABELS "smoke;contract")

add_test(
    NAME model_config_cli_integration
    COMMAND "${Python3_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/tests/scripts/model_config_cli_tests.py"
        --mint $<TARGET_FILE:mint>
)
set_tests_properties(model_config_cli_integration PROPERTIES
    LABELS "integration;contract;model-config"
    TIMEOUT 75
)

add_test(
    NAME mint_provider_profile
    COMMAND $<TARGET_FILE:mint> provider
        --config "${PROJECT_SOURCE_DIR}/configs/providers/groq-chat.json"
        --state-dir "${PROJECT_BINARY_DIR}/tests/state/mint-provider-profile"
        --log-file-level off
        --json
)
set_tests_properties(
    mint_provider_profile
    PROPERTIES
        LABELS "provider;smoke"
        PASS_REGULAR_EXPRESSION "\"provider\":\"groq\""
)
