add_library(mint_tools STATIC
    change_set.cpp
    change_transaction.cpp
    file_editor.cpp
    file_support.cpp
    tool_catalog.cpp
    tool_registry.cpp
    tool_support.cpp
    workspace_tools.cpp
)
add_library(mint::tools ALIAS mint_tools)

mint_configure_target(mint_tools)
target_link_libraries(mint_tools
    PUBLIC
        mint::domain
        mint::infrastructure
        mint::runtime
)
