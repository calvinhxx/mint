include_guard(GLOBAL)

set(MINT_FORMAT_EXECUTABLE "" CACHE FILEPATH "Path to the clang-format executable")
mark_as_advanced(MINT_FORMAT_EXECUTABLE)

function(mint_add_developer_tools)
    set(format_executable "${MINT_FORMAT_EXECUTABLE}")
    if(NOT format_executable AND APPLE)
        execute_process(
            COMMAND xcrun --find clang-format
            RESULT_VARIABLE xcrun_result
            OUTPUT_VARIABLE format_executable
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
        if(NOT xcrun_result EQUAL 0)
            set(format_executable "")
        endif()
    endif()
    if(NOT format_executable)
        find_program(format_executable NAMES clang-format NO_CACHE)
    endif()
    if(NOT format_executable)
        message(STATUS "clang-format not found; mint format targets are disabled")
        return()
    endif()

    file(GLOB_RECURSE mint_format_files CONFIGURE_DEPENDS
        "${PROJECT_SOURCE_DIR}/include/*.hpp"
        "${PROJECT_SOURCE_DIR}/src/*.cpp"
        "${PROJECT_SOURCE_DIR}/src/*.hpp"
        "${PROJECT_SOURCE_DIR}/tests/*.cpp"
        "${PROJECT_SOURCE_DIR}/tests/*.hpp"
    )
    list(SORT mint_format_files)

    add_custom_target(mint-format
        COMMAND "${format_executable}" -i ${mint_format_files}
        COMMENT "Formatting mint C++ sources"
        VERBATIM
    )
    add_custom_target(mint-format-check
        COMMAND "${format_executable}" --dry-run --Werror ${mint_format_files}
        COMMENT "Checking mint C++ source formatting"
        VERBATIM
    )

    if(PROJECT_IS_TOP_LEVEL)
        add_custom_target(format DEPENDS mint-format)
        add_custom_target(format-check DEPENDS mint-format-check)
    endif()
endfunction()
