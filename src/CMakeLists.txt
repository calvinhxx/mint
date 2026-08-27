add_subdirectory(domain)
add_subdirectory(runtime)
add_subdirectory(infrastructure)
add_subdirectory(tools)
add_subdirectory(application)

add_library(mint_core INTERFACE)
add_library(mint::core ALIAS mint_core)
target_link_libraries(mint_core INTERFACE mint::application)

if(MINT_BUILD_CLI)
    add_subdirectory(cli)
endif()
