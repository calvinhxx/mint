add_subdirectory(platform)
add_subdirectory(localization)
add_subdirectory(domain)

add_library(mint_ports INTERFACE)
add_library(mint::ports ALIAS mint_ports)
target_compile_features(mint_ports INTERFACE cxx_std_20)
target_include_directories(mint_ports INTERFACE
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
)
target_link_libraries(mint_ports INTERFACE mint::domain)

add_subdirectory(runtime)
add_subdirectory(infrastructure)
add_subdirectory(tools)
add_subdirectory(application)

add_library(mint_core INTERFACE)
add_library(mint::core ALIAS mint_core)
target_link_libraries(mint_core INTERFACE
    mint::localization
    mint::agent
    mint::command
    mint::filesystem
    mint::logging
    mint::model
    mint::model_settings
    mint::persistence
    mint::project
    mint::runtime
    mint::tools
)
