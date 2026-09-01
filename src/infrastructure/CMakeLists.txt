add_library(mint_filesystem STATIC
    filesystem/output_path.cpp
    filesystem/private_path.cpp
)
add_library(mint::filesystem ALIAS mint_filesystem)
mint_configure_target(mint_filesystem)

if(WIN32)
    target_link_libraries(mint_filesystem PRIVATE advapi32)
endif()

add_library(mint_logging STATIC
    logging/diagnostic_log.cpp
    logging/diagnostic_log_file.cpp
    logging/diagnostic_log_schema.cpp
)
add_library(mint::logging ALIAS mint_logging)
mint_configure_target(mint_logging)
target_include_directories(mint_logging PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(mint_logging
    PUBLIC
        mint::domain
    PRIVATE
        mint::filesystem
        mint::runtime
        spdlog::spdlog
)

if(WIN32)
    set(mint_command_process_sources
        command/command_appcontainer_windows.cpp
        command/command_process_windows.cpp
    )
else()
    set(mint_command_process_sources
        command/command_process.cpp
        command/command_process_tree.cpp
    )
endif()

add_library(mint_command STATIC
    ${mint_command_process_sources}
    command/command_runner.cpp
    command/command_resource_monitor.cpp
    command/command_sandbox.cpp
)
add_library(mint::command ALIAS mint_command)
mint_configure_target(mint_command)
target_link_libraries(mint_command
    PUBLIC
        mint::domain
        mint::runtime
    PRIVATE
        mint::logging
)

if(WIN32)
    target_link_libraries(mint_command PRIVATE advapi32 ole32 userenv)
endif()

add_library(mint_model STATIC
    model/config.cpp
    model/demo_model_client.cpp
    model/model_client.cpp
    model/model_http_transport.cpp
    model/model_provider_profile.cpp
    model/model_protocol.cpp
    model/model_protocol_anthropic.cpp
    model/model_protocol_common.cpp
    model/model_protocol_request.cpp
    model/model_protocol_response.cpp
    model/model_protocol_stream.cpp
    model/model_request.cpp
)
add_library(mint::model ALIAS mint_model)
mint_configure_target(mint_model)
target_link_libraries(mint_model
    PUBLIC
        mint::ports
    PRIVATE
        CURL::libcurl
        mint::logging
        mint::runtime
)

add_library(mint_persistence STATIC
    persistence/change_transaction_store.cpp
    persistence/event_log.cpp
    persistence/project_store.cpp
    persistence/session_store.cpp
)
add_library(mint::persistence ALIAS mint_persistence)
mint_configure_target(mint_persistence)
target_include_directories(mint_persistence PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(mint_persistence
    PUBLIC
        mint::domain
        mint::ports
    PRIVATE
        mint::filesystem
        mint::runtime
)

# Stable facade for callers that intentionally use every infrastructure adapter.
add_library(mint_infrastructure INTERFACE)
add_library(mint::infrastructure ALIAS mint_infrastructure)
target_link_libraries(mint_infrastructure INTERFACE
    mint::command
    mint::filesystem
    mint::logging
    mint::model
    mint::persistence
)
