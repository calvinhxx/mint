add_library(mint_cli STATIC
    agent/agent_command.cpp
    agent/agent_command_config.cpp
    agent/agent_command_io.cpp
    agent/agent_event_router.cpp
    command_line.cpp
    project/project_commands.cpp
    provider/provider_acceptance.cpp
    provider/provider_command.cpp
    support/console.cpp
    support/diagnostic_logging.cpp
)

mint_configure_target(mint_cli)
target_include_directories(mint_cli PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)
target_link_libraries(mint_cli
    PUBLIC
        mint::agent
        mint::domain
        mint::logging
        mint::localization
        mint::model
        mint::persistence
        mint::project
        mint::runtime
        mint::tools
)

add_executable(mint main.cpp)
mint_configure_target(mint)
target_include_directories(mint PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)
target_link_libraries(mint PRIVATE mint_cli)
set_target_properties(mint PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}"
)
