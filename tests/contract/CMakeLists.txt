mint_add_gtest(mint_policy_recovery_tests
    SOURCES policy_and_recovery_tests.cpp
    LIBRARIES mint::core
    TEST_PREFIX "contract.policy."
    LABELS contract policy recovery
)

mint_add_gtest(mint_project_workflow_tests
    GTEST_MAIN
    SOURCES project_workflow_tests.cpp
    LIBRARIES mint::core
    TEST_PREFIX "contract.project."
    LABELS contract project
)

mint_add_gtest(mint_provider_protocol_tests
    GTEST_MAIN
    SOURCES provider_protocol_tests.cpp
    LIBRARIES mint_cli mint_test_support
    INCLUDE_DIRECTORIES
        "${PROJECT_SOURCE_DIR}/src/cli"
        "${PROJECT_SOURCE_DIR}/src/core/infrastructure"
    TEST_PREFIX "contract.protocol."
    LABELS contract provider acceptance
)

mint_add_gtest(mint_provider_tests
    GTEST_MAIN
    SOURCES
        model_usage_tests.cpp
        provider_acceptance_tests.cpp
        provider_catalog_tests.cpp
        provider_profile_tests.cpp
    LIBRARIES mint_cli mint_test_support
    INCLUDE_DIRECTORIES
        "${PROJECT_SOURCE_DIR}/src/core/application"
        "${PROJECT_SOURCE_DIR}/src/cli"
        "${PROJECT_SOURCE_DIR}/src/core/infrastructure"
    TEST_PREFIX "contract.provider."
    LABELS contract provider acceptance
)
target_compile_definitions(mint_provider_tests PRIVATE
    MINT_PROVIDER_CONFIG_DIR="${PROJECT_SOURCE_DIR}/configs/providers"
)
