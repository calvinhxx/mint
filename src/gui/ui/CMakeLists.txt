add_library(mint_gui_ui STATIC
    components/approval_panel.cpp
    components/conversation_transcript.cpp
    components/evidence_panel.cpp
    components/fox_mark.cpp
    components/mint_title_bar.cpp
    components/run_conversation.cpp
    components/settings_panel.cpp
    components/task_composer.cpp
    components/task_sidebar.cpp
    components/theme_choice_button.cpp
    components/title_bar_surface.cpp
    components/underline_free_input.cpp
    components/workspace_split_view.cpp
    delegates/run_timeline_delegate.cpp
    delegates/task_item_delegate.cpp
    models/run_timeline_model.cpp
    models/task_list_model.cpp
    pages/task_workspace_page.cpp
    presentation/run_event_presentation.cpp
    resources/mint_gui.qrc
    shell/mint_window.cpp
    support/gui_resources.cpp
    theme/mint_theme.cpp
)
add_library(mint::gui_ui ALIAS mint_gui_ui)

set_target_properties(mint_gui_ui PROPERTIES AUTOMOC ON AUTORCC ON)
mint_configure_target(mint_gui_ui)
target_include_directories(mint_gui_ui PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src/gui>
)
target_link_libraries(mint_gui_ui
    PUBLIC
        mint::gui_application
        mint::localization
        FluentQt::FluentQt
        Qt6::Core
        Qt6::Gui
        Qt6::Svg
        Qt6::Widgets
)
