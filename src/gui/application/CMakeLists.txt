add_library(mint_gui_application STATIC
    model_settings_controller.cpp
    preferences/preferences_controller.cpp
    task_organization.cpp
    task_workspace_controller.cpp
    workspaces/workspace_catalog.cpp
)
add_library(mint::gui_application ALIAS mint_gui_application)

mint_configure_target(mint_gui_application)
target_include_directories(mint_gui_application PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src/gui>
)
target_link_libraries(mint_gui_application PUBLIC mint::localization)
