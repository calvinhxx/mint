add_test(
    NAME architecture_console_boundary
    COMMAND "${CMAKE_COMMAND}"
        -DSOURCE_DIR=${PROJECT_SOURCE_DIR}
        -P "${CMAKE_CURRENT_LIST_DIR}/VerifyConsoleBoundary.cmake"
)
set_tests_properties(architecture_console_boundary PROPERTIES LABELS "architecture;contract")

add_test(
    NAME architecture_application_boundary
    COMMAND "${CMAKE_COMMAND}"
        -DSOURCE_DIR=${PROJECT_SOURCE_DIR}
        -P "${CMAKE_CURRENT_LIST_DIR}/VerifyApplicationBoundary.cmake"
)
set_tests_properties(architecture_application_boundary PROPERTIES LABELS "architecture;contract")

add_test(
    NAME architecture_module_layout
    COMMAND "${CMAKE_COMMAND}"
        -DSOURCE_DIR=${PROJECT_SOURCE_DIR}
        -P "${CMAKE_CURRENT_LIST_DIR}/VerifyModuleLayout.cmake"
)
set_tests_properties(architecture_module_layout PROPERTIES LABELS "architecture;contract")

add_test(
    NAME architecture_desktop_boundary
    COMMAND "${CMAKE_COMMAND}"
        -DSOURCE_DIR=${PROJECT_SOURCE_DIR}
        -P "${CMAKE_CURRENT_LIST_DIR}/VerifyDesktopBoundary.cmake"
)
set_tests_properties(architecture_desktop_boundary PROPERTIES LABELS "architecture;contract;desktop")

if(APPLE)
    add_test(
        NAME macos_development_app_contract
        COMMAND "${CMAKE_COMMAND}"
            "-DMINT_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
            "-DMINT_TEST_DIRECTORY=${PROJECT_BINARY_DIR}/tests/macos-development-app-contract"
            "-DMINT_TEST_GENERATOR=${CMAKE_GENERATOR}"
            -P "${CMAKE_CURRENT_LIST_DIR}/VerifyMacOSDevelopmentApp.cmake"
    )
    set_tests_properties(macos_development_app_contract PROPERTIES
        LABELS "desktop;build;contract"
        TIMEOUT 60
    )
endif()
