include_guard(GLOBAL)

function(mint_add_macos_development_app gui_target cli_target)
    if(NOT TARGET "${gui_target}" OR NOT TARGET "${cli_target}")
        message(FATAL_ERROR "The Mint development app requires its GUI and CLI targets")
    endif()
    find_program(MINT_CODESIGN_EXECUTABLE NAMES codesign REQUIRED)
    set(MINT_APP_VERSION "${PROJECT_VERSION}")
    set(MINT_APP_MINIMUM_SYSTEM_VERSION "")
    if(CMAKE_OSX_DEPLOYMENT_TARGET)
        set(MINT_APP_MINIMUM_SYSTEM_VERSION
            "    <key>LSMinimumSystemVersion</key>\n    <string>${CMAKE_OSX_DEPLOYMENT_TARGET}</string>"
        )
    endif()
    set(app_plist "${CMAKE_CURRENT_BINARY_DIR}/MintDevelopmentAppInfo.plist")
    configure_file(
        "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/MintDevelopmentAppInfo.plist.in"
        "${app_plist}" @ONLY
    )

    # EN: Always synchronize both executables, even when only the CLI changed or the app was deleted.
    # ZH-CN: 每次都同步两个程序，覆盖只更新 CLI 或手动删除 App 的情况。
    add_custom_target(mint_gui_app
        COMMAND "${CMAKE_COMMAND}"
            "-DMINT_GUI_EXECUTABLE=$<TARGET_FILE:${gui_target}>"
            "-DMINT_CLI_EXECUTABLE=$<TARGET_FILE:${cli_target}>"
            "-DMINT_APP_BUNDLE=${PROJECT_BINARY_DIR}/Mint.app"
            "-DMINT_APP_INFO_PLIST=${app_plist}"
            "-DMINT_CODESIGN_EXECUTABLE=${MINT_CODESIGN_EXECUTABLE}"
            -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/BuildMacOSDevelopmentApp.cmake"
        COMMENT "Create the signed local development Mint.app"
        VERBATIM
    )
    add_dependencies(mint_gui_app "${gui_target}" "${cli_target}")
endfunction()
