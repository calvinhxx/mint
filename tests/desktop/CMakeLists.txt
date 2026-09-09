find_package(Qt6 6.5 REQUIRED COMPONENTS Test)

add_executable(mint_desktop_process_fixture ../support/desktop_process_fixture.cpp)
mint_configure_target(mint_desktop_process_fixture)

mint_add_gtest(mint_desktop_tests
    SOURCES
        desktop_branding_tests.cpp
        desktop_controller_tests.cpp
        desktop_input_tests.cpp
        desktop_model_tests.cpp
        desktop_preferences_tests.cpp
        desktop_process_tests.cpp
        desktop_protocol_tests.cpp
        desktop_sidebar_interaction_tests.cpp
        desktop_startup_tests.cpp
        ${PROJECT_SOURCE_DIR}/src/gui/app/gui_options.cpp
        desktop_task_organization_tests.cpp
        desktop_test_main.cpp
        desktop_timeline_tests.cpp
    LIBRARIES
        mint::gui_cli_adapter
        mint::gui_settings
        mint::gui_ui
        Qt6::Core
        Qt6::Test
    INCLUDE_DIRECTORIES "${PROJECT_SOURCE_DIR}/src/gui"
    TEST_PREFIX "desktop."
    LABELS desktop integration
)
target_compile_definitions(mint_desktop_tests PRIVATE MINT_TEST_CLI_PATH="$<TARGET_FILE:mint>")
target_compile_definitions(mint_desktop_tests PRIVATE
    MINT_TEST_PROCESS_FIXTURE_PATH="$<TARGET_FILE:mint_desktop_process_fixture>")
add_dependencies(mint_desktop_tests mint_desktop_process_fixture)

mint_add_gtest(mint_desktop_custom_model_tests
    GTEST_MAIN
    SOURCES
        desktop_custom_model_tests.cpp
        desktop_shared_config_integration_tests.cpp
    LIBRARIES mint::gui_settings Qt6::Core Qt6::Test
    INCLUDE_DIRECTORIES "${PROJECT_SOURCE_DIR}/src/gui"
    TEST_PREFIX "desktop.custom-model."
    LABELS desktop integration
)
target_compile_definitions(mint_desktop_custom_model_tests PRIVATE
    MINT_TEST_CLI_PATH="$<TARGET_FILE:mint>")
add_dependencies(mint_desktop_custom_model_tests mint)
