if(NOT DEFINED SOURCE_DIR)
    message(FATAL_ERROR "SOURCE_DIR is required")
endif()

set(violations)

file(GLOB_RECURSE core_sources
    LIST_DIRECTORIES FALSE
    "${SOURCE_DIR}/src/core/*.cpp"
    "${SOURCE_DIR}/src/core/*.hpp"
)
foreach(source IN LISTS core_sources)
    file(READ "${source}" contents)
    if(contents MATCHES "#include[ \t]+[<\"](Q[A-Za-z]+|FluentQt/|gui/)")
        file(RELATIVE_PATH relative "${SOURCE_DIR}" "${source}")
        list(APPEND violations "${relative}: core must not depend on CLI or GUI frameworks")
    endif()
endforeach()

file(GLOB_RECURSE cli_sources
    LIST_DIRECTORIES FALSE
    "${SOURCE_DIR}/src/cli/*.cpp"
    "${SOURCE_DIR}/src/cli/*.hpp"
)
foreach(source IN LISTS cli_sources)
    file(READ "${source}" contents)
    if(contents MATCHES "#include[ \t]+[<\"](Q[A-Za-z]+|FluentQt/|gui/)")
        file(RELATIVE_PATH relative "${SOURCE_DIR}" "${source}")
        list(APPEND violations "${relative}: CLI must not depend on GUI")
    endif()
endforeach()

file(GLOB_RECURSE desktop_application_sources
    LIST_DIRECTORIES FALSE
    "${SOURCE_DIR}/src/gui/application/*.cpp"
    "${SOURCE_DIR}/src/gui/application/*.hpp"
)
foreach(source IN LISTS desktop_application_sources)
    file(READ "${source}" contents)
    if(contents MATCHES "#include[ \t]+[<\"](Q[A-Za-z]+|infrastructure/|nlohmann/)" OR
       contents MATCHES "mint/domain/model.hpp" OR
       contents MATCHES "(^|[^A-Za-z0-9_])Json([^A-Za-z0-9_]|$)")
        file(RELATIVE_PATH relative "${SOURCE_DIR}" "${source}")
        list(APPEND violations "${relative}: desktop application leaked a UI or adapter detail")
    endif()
endforeach()

file(GLOB_RECURSE desktop_ui_sources
    LIST_DIRECTORIES FALSE
    "${SOURCE_DIR}/src/gui/ui/*.cpp"
    "${SOURCE_DIR}/src/gui/ui/*.hpp"
)
foreach(source IN LISTS desktop_ui_sources)
    file(READ "${source}" contents)
    if(contents MATCHES "#include[ \t]+[<\"]infrastructure/" OR
       contents MATCHES "QProcess" OR
       contents MATCHES "nlohmann" OR
       contents MATCHES "(^|[^A-Za-z0-9_])Json([^A-Za-z0-9_]|$)")
        file(RELATIVE_PATH relative "${SOURCE_DIR}" "${source}")
        list(APPEND violations "${relative}: UI must depend on typed desktop application APIs")
    endif()
endforeach()

file(GLOB_RECURSE desktop_adapter_sources
    LIST_DIRECTORIES FALSE
    "${SOURCE_DIR}/src/gui/infrastructure/*.cpp"
    "${SOURCE_DIR}/src/gui/infrastructure/*.hpp"
)
foreach(source IN LISTS desktop_adapter_sources)
    file(READ "${source}" contents)
    if(contents MATCHES "#include[ \t]+[<\"]ui/")
        file(RELATIVE_PATH relative "${SOURCE_DIR}" "${source}")
        list(APPEND violations "${relative}: desktop adapter must not depend on UI")
    endif()
endforeach()

file(READ "${SOURCE_DIR}/src/gui/ui/CMakeLists.txt" ui_cmake)
if(ui_cmake MATCHES "mint::gui_cli_adapter" OR NOT ui_cmake MATCHES "mint::gui_application")
    list(APPEND violations "src/gui/ui/CMakeLists.txt: UI target dependency is not isolated")
endif()

if(violations)
    list(REMOVE_DUPLICATES violations)
    list(JOIN violations ", " violation_list)
    message(FATAL_ERROR "Desktop boundary violations: ${violation_list}")
endif()
