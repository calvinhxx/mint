if(NOT DEFINED SOURCE_DIR)
    message(FATAL_ERROR "SOURCE_DIR is required")
endif()

file(GLOB_RECURSE application_sources
    LIST_DIRECTORIES FALSE
    "${SOURCE_DIR}/include/mint/application/*.hpp"
    "${SOURCE_DIR}/src/application/*.cpp"
    "${SOURCE_DIR}/src/application/*.hpp"
)

set(violations)
foreach(source IN LISTS application_sources)
    file(READ "${source}" contents)
    string(REGEX MATCH "#include[ \t]+\"mint/(infrastructure|tools)/" forbidden_include
                 "${contents}")
    if(forbidden_include)
        file(RELATIVE_PATH relative "${SOURCE_DIR}" "${source}")
        list(APPEND violations "${relative}")
    endif()
endforeach()

file(READ "${SOURCE_DIR}/src/application/CMakeLists.txt" application_cmake)
string(REGEX MATCH "mint::(infrastructure|tools)" forbidden_link "${application_cmake}")
if(forbidden_link)
    list(APPEND violations "src/application/CMakeLists.txt")
endif()

if(violations)
    list(JOIN violations ", " violation_list)
    message(FATAL_ERROR
        "Application must depend on ports, not concrete adapters: ${violation_list}"
    )
endif()
