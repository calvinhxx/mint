add_subdirectory(core)

if(MINT_BUILD_CLI)
    add_subdirectory(cli)
endif()

if(MINT_BUILD_GUI)
    add_subdirectory(gui)
endif()
