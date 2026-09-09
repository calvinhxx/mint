mint_add_gtest(mint_tests
    SOURCES
        agent_config_paths_tests.cpp
        agent_context_tests.cpp
        agent_loop_tests.cpp
        agent_session_tests.cpp
        agent_token_budget_tests.cpp
        agent_verification_tests.cpp
        change_transaction_tests.cpp
        command_change_tracking_tests.cpp
        command_runner_tests.cpp
        model_client_tests.cpp
        runtime_files_tests.cpp
        terminal_output_tests.cpp
        tool_registry_tests.cpp
        workspace_scan_tests.cpp
    LIBRARIES mint::core mint_cli mint_test_support
    INCLUDE_DIRECTORIES
        "${PROJECT_SOURCE_DIR}/src/core/application"
        "${PROJECT_SOURCE_DIR}/src/cli"
        "${PROJECT_SOURCE_DIR}/src/core/tools"
    TEST_PREFIX "integration."
    LABELS integration
)
target_compile_definitions(mint_tests PRIVATE MINT_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
