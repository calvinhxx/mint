add_library(mint_tools STATIC
    changes/change_set.cpp
    changes/change_transaction.cpp
    changes/workspace_change_tracker.cpp
    registry/tool_arguments.cpp
    registry/tool_catalog.cpp
    registry/tool_registry.cpp
    registry/tool_registry_command.cpp
    workspace/file_editor.cpp
    workspace/file_support.cpp
    workspace/path_identity.cpp
    workspace/sensitive_path.cpp
    workspace/workspace_support.cpp
    workspace/workspace_tools.cpp
)
add_library(mint::tools ALIAS mint_tools)

mint_configure_target(mint_tools)
target_include_directories(mint_tools PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)
target_link_libraries(mint_tools
    PUBLIC
        mint::domain
        mint::ports
        mint::runtime
    PRIVATE
        mint::command
        mint::localization
        mint::logging
        mint::persistence
)
