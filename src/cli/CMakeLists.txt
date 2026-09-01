add_library(mint_cli STATIC
    agent_command.cpp
    agent_command_config.cpp
    agent_event_router.cpp
    agent_command_io.cpp
    command_line.cpp
    console.cpp
    diagnostic_logging.cpp
    project_commands.cpp
    provider_acceptance.cpp
    provider_command.cpp
)

mint_configure_target(mint_cli)
target_include_directories(mint_cli PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)
target_link_libraries(mint_cli
    PUBLIC
        mint::agent
        mint::domain
        mint::logging
        mint::model
        mint::persistence
        mint::project
        mint::runtime
        mint::tools
)

add_executable(mint main.cpp)
mint_configure_target(mint)
target_link_libraries(mint PRIVATE mint_cli)
set_target_properties(mint PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}"
)
