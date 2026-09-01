if(NOT DEFINED SOURCE_DIR)
    message(FATAL_ERROR "SOURCE_DIR is required")
endif()

file(GLOB_RECURSE production_sources
    LIST_DIRECTORIES FALSE
    "${SOURCE_DIR}/include/*.hpp"
    "${SOURCE_DIR}/src/*.cpp"
    "${SOURCE_DIR}/src/*.hpp"
)

set(system_console_adapter "${SOURCE_DIR}/src/cli/support/console.cpp")
set(violations)
foreach(source IN LISTS production_sources)
    if(source STREQUAL system_console_adapter)
        continue()
    endif()

    file(READ "${source}" contents)
    string(REGEX MATCH "std::(cin|cout|cerr|clog|wcin|wcout|wcerr|wclog)" stream_usage
                 "${contents}")
    string(REGEX MATCH "(printf|fprintf|puts|fputs|fwrite)[ \t\r\n]*\\(" stdio_usage
                 "${contents}")
    if(stream_usage OR stdio_usage)
        file(RELATIVE_PATH relative "${SOURCE_DIR}" "${source}")
        list(APPEND violations "${relative}")
    endif()
endforeach()

if(violations)
    list(JOIN violations ", " violation_list)
    message(FATAL_ERROR
        "Direct process I/O is restricted to src/cli/support/console.cpp: ${violation_list}"
    )
endif()
