if(WIN32)
    set(mint_command_process_source command_process_windows.cpp)
else()
    set(mint_command_process_source command_process.cpp)
endif()

add_library(mint_infrastructure STATIC
    ${mint_command_process_source}
    command_runner.cpp
    command_sandbox.cpp
    config.cpp
    demo_model_client.cpp
    diagnostic_log.cpp
    event_log.cpp
    model_client.cpp
    model_http_transport.cpp
    model_protocol.cpp
    model_request.cpp
    output_path.cpp
    project_store.cpp
    session_store.cpp
)
add_library(mint::infrastructure ALIAS mint_infrastructure)

mint_configure_target(mint_infrastructure)
target_link_libraries(mint_infrastructure
    PUBLIC
        mint::domain
        mint::runtime
    PRIVATE
        CURL::libcurl
        spdlog::spdlog
)
