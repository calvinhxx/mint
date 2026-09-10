if(WIN32)
    set(mint_private_path_source filesystem/native/private_path_windows.cpp)
else()
    set(mint_private_path_source filesystem/native/private_path_posix.cpp)
endif()

add_library(mint_filesystem STATIC
    ${mint_private_path_source}
    filesystem/output_path.cpp
    filesystem/private_path.cpp
)
add_library(mint::filesystem ALIAS mint_filesystem)
mint_configure_target(mint_filesystem)
target_link_libraries(mint_filesystem PRIVATE mint::localization)

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
        mint::platform
        mint::localization
        mint::runtime
        spdlog::spdlog
)

if(WIN32)
    set(mint_command_native_sources
        command/native/command_appcontainer_windows.cpp
        command/native/command_process_windows.cpp
        command/native/command_sandbox_windows.cpp
    )
else()
    set(mint_command_native_sources
        command/native/command_process_posix.cpp
        command/native/command_process_tree_posix.cpp
        command/native/command_sandbox_posix.cpp
    )
    if(APPLE)
        list(APPEND mint_command_native_sources command/native/command_sandbox_macos.cpp)
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        list(APPEND mint_command_native_sources command/native/command_sandbox_linux.cpp)
    else()
        list(APPEND mint_command_native_sources command/native/command_sandbox_unsupported.cpp)
    endif()
endif()

add_library(mint_command STATIC
    ${mint_command_native_sources}
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
        mint::localization
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
        mint::localization
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
        mint::localization
        mint::platform
        mint::runtime
)

add_library(mint_model_settings STATIC
    settings/model_config_service.cpp
    settings/model_settings_credentials.cpp
    settings/model_settings_files.cpp
    settings/model_settings_profile.cpp
    settings/model_settings_transactions.cpp
)
add_library(mint::model_settings ALIAS mint_model_settings)
mint_configure_target(mint_model_settings)
target_include_directories(mint_model_settings PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(mint_model_settings
    PUBLIC mint::model
    PRIVATE mint::filesystem mint::persistence mint::platform
)
