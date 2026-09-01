add_subdirectory(localization)
add_subdirectory(domain)
add_subdirectory(ports)
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
    mint::persistence
    mint::project
    mint::runtime
    mint::tools
)

if(MINT_BUILD_CLI)
    add_subdirectory(cli)
endif()
