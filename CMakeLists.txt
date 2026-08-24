cmake_minimum_required(VERSION 3.24)

project(aiagent VERSION 1.4.0 LANGUAGES CXX)

include(FetchContent)
include(cmake/ProjectOptions.cmake)

find_package(CURL REQUIRED)
find_package(nlohmann_json 3.11 QUIET)

if(NOT nlohmann_json_FOUND)
    FetchContent_Declare(
        nlohmann_json
        URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(nlohmann_json)
endif()

add_library(aiagent_domain STATIC
    src/domain/change_journal.cpp
    src/domain/task_policy.cpp
)
aiagent_configure_target(aiagent_domain)
target_link_libraries(aiagent_domain PUBLIC nlohmann_json::nlohmann_json)

add_library(aiagent_runtime STATIC
    src/runtime/task_control.cpp
)
aiagent_configure_target(aiagent_runtime)

add_library(aiagent_infrastructure STATIC
    src/infrastructure/command_runner.cpp
    src/infrastructure/config.cpp
    src/infrastructure/event_log.cpp
    src/infrastructure/model_client.cpp
    src/infrastructure/model_protocol.cpp
    src/infrastructure/project_store.cpp
    src/infrastructure/session_store.cpp
)
aiagent_configure_target(aiagent_infrastructure)
target_link_libraries(aiagent_infrastructure
    PUBLIC aiagent_domain aiagent_runtime
    PRIVATE CURL::libcurl
)

add_library(aiagent_tools STATIC
    src/tools/change_set.cpp
    src/tools/file_support.cpp
    src/tools/tool_registry.cpp
)
aiagent_configure_target(aiagent_tools)
target_link_libraries(aiagent_tools
    PUBLIC aiagent_domain aiagent_infrastructure aiagent_runtime
)

add_library(aiagent_application STATIC
    src/application/agent.cpp
    src/application/project_service.cpp
)
aiagent_configure_target(aiagent_application)
target_link_libraries(aiagent_application
    PUBLIC aiagent_domain aiagent_infrastructure aiagent_runtime aiagent_tools
)

# Stable aggregate target for embedders that do not need to know layer names.
add_library(aiagent_core INTERFACE)
target_link_libraries(aiagent_core INTERFACE aiagent_application)

add_executable(aiagent
    src/cli/command_line.cpp
    src/cli/main.cpp
    src/cli/project_commands.cpp
)
aiagent_configure_target(aiagent)
target_link_libraries(aiagent PRIVATE aiagent_core)

include(cmake/DeveloperTools.cmake)

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
