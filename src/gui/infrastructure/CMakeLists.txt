add_library(mint_gui_cli_adapter STATIC
    cli/mint_cli_command_builder.cpp
    cli/mint_cli_process.cpp
    cli/mint_cli_protocol.cpp
    cli/run_event_stream.cpp
    cli/task_history_reader.cpp
)
add_library(mint::gui_cli_adapter ALIAS mint_gui_cli_adapter)

mint_configure_target(mint_gui_cli_adapter)
target_include_directories(mint_gui_cli_adapter PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src/gui>
)
target_link_libraries(mint_gui_cli_adapter
    PUBLIC
        mint::gui_application
        Qt6::Concurrent
        Qt6::Core
    PRIVATE
        nlohmann_json::nlohmann_json
)

add_library(mint_gui_settings STATIC
    settings/shared_model_settings_operations.cpp
    settings/shared_model_settings_runtime.cpp
    settings/qsettings_preferences_repository.cpp
    settings/qsettings_task_organization_repository.cpp
    settings/qsettings_workspace_catalog_repository.cpp
)
add_library(mint::gui_settings ALIAS mint_gui_settings)

mint_configure_target(mint_gui_settings)
target_include_directories(mint_gui_settings PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src/gui>
)
target_link_libraries(mint_gui_settings
    PUBLIC
        mint::gui_application
        mint::model_settings
        Qt6::Core
        Qt6::Concurrent
    PRIVATE
        nlohmann_json::nlohmann_json
)
