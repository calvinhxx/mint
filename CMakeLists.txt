cmake_minimum_required(VERSION 3.24)

project(mint VERSION 1.4.0 DESCRIPTION "Lightweight general AI agent toolkit" LANGUAGES CXX)

include(cmake/ProjectOptions.cmake)

find_package(CURL REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)

add_library(mint_domain STATIC
    src/domain/change_journal.cpp
    src/domain/runtime_settings.cpp
    src/domain/task_policy.cpp
)
mint_configure_target(mint_domain)
target_link_libraries(mint_domain PUBLIC nlohmann_json::nlohmann_json)

add_library(mint_runtime STATIC
    src/runtime/task_control.cpp
)
mint_configure_target(mint_runtime)

add_library(mint_infrastructure STATIC
    src/infrastructure/command_process.cpp
    src/infrastructure/command_runner.cpp
    src/infrastructure/command_sandbox.cpp
    src/infrastructure/config.cpp
    src/infrastructure/demo_model_client.cpp
    src/infrastructure/diagnostic_log.cpp
    src/infrastructure/event_log.cpp
    src/infrastructure/model_client.cpp
    src/infrastructure/model_http_transport.cpp
    src/infrastructure/model_protocol.cpp
    src/infrastructure/model_request.cpp
    src/infrastructure/output_path.cpp
    src/infrastructure/project_store.cpp
    src/infrastructure/session_store.cpp
)
mint_configure_target(mint_infrastructure)
target_link_libraries(mint_infrastructure
    PUBLIC mint_domain mint_runtime
    PRIVATE CURL::libcurl spdlog::spdlog
)

add_library(mint_tools STATIC
    src/tools/change_set.cpp
    src/tools/file_editor.cpp
    src/tools/file_support.cpp
    src/tools/tool_catalog.cpp
    src/tools/tool_support.cpp
    src/tools/tool_registry.cpp
    src/tools/workspace_tools.cpp
)
mint_configure_target(mint_tools)
target_link_libraries(mint_tools
    PUBLIC mint_domain mint_infrastructure mint_runtime
)

add_library(mint_application STATIC
    src/application/agent.cpp
    src/application/agent_checkpoint.cpp
    src/application/agent_context.cpp
    src/application/agent_cycle.cpp
    src/application/agent_loop.cpp
    src/application/agent_prompt.cpp
    src/application/agent_state.cpp
    src/application/project_service.cpp
)
mint_configure_target(mint_application)
target_link_libraries(mint_application
    PUBLIC mint_domain mint_infrastructure mint_runtime mint_tools
)

# Stable aggregate target for embedders that do not need to know layer names.
add_library(mint_core INTERFACE)
target_link_libraries(mint_core INTERFACE mint_application)

add_library(mint_cli STATIC
    src/cli/agent_command.cpp
    src/cli/agent_command_config.cpp
    src/cli/agent_command_io.cpp
    src/cli/command_line.cpp
    src/cli/console.cpp
    src/cli/project_commands.cpp
)
mint_configure_target(mint_cli)
target_include_directories(mint_cli PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src/cli>)
target_link_libraries(mint_cli PUBLIC mint_core)

add_executable(mint src/cli/main.cpp)
mint_configure_target(mint)
target_link_libraries(mint PRIVATE mint_cli)

include(cmake/DeveloperTools.cmake)

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
