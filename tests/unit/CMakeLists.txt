mint_add_gtest(mint_unit_tests
    GTEST_MAIN
    SOURCES
        console_tests.cpp
        command_line_tests.cpp
        diagnostic_log_tests.cpp
        interaction_protocol_tests.cpp
        localization_tests.cpp
        model_config_command_tests.cpp
        path_tests.cpp
        runtime_settings_tests.cpp
    LIBRARIES mint_cli
    INCLUDE_DIRECTORIES
        "${PROJECT_SOURCE_DIR}/src/cli"
        "${PROJECT_SOURCE_DIR}/src/core/infrastructure"
    TEST_PREFIX "unit."
    LABELS unit
)

mint_add_gtest(mint_model_config_tests
    GTEST_MAIN
    SOURCES model_config_service_tests.cpp
    LIBRARIES mint::model_settings
    INCLUDE_DIRECTORIES "${PROJECT_SOURCE_DIR}/tests/support"
    TEST_PREFIX "unit."
    LABELS unit model-config
)
