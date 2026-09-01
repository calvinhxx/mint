add_library(mint_tools STATIC
    change_set.cpp
    change_transaction.cpp
    file_editor.cpp
    file_support.cpp
    path_identity.cpp
    sensitive_path.cpp
    tool_catalog.cpp
    tool_registry.cpp
    tool_registry_command.cpp
    tool_support.cpp
    workspace_change_tracker.cpp
    workspace_tools.cpp
)
add_library(mint::tools ALIAS mint_tools)

mint_configure_target(mint_tools)
target_link_libraries(mint_tools
    PUBLIC
        mint::domain
        mint::ports
        mint::runtime
    PRIVATE
        mint::command
        mint::logging
        mint::persistence
)
