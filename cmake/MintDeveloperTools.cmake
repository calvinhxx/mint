include_guard(GLOBAL)

function(mint_add_developer_tools)
    find_program(MINT_CLANG_FORMAT_EXECUTABLE NAMES clang-format)
    if(NOT MINT_CLANG_FORMAT_EXECUTABLE)
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
        COMMAND "${MINT_CLANG_FORMAT_EXECUTABLE}" -i ${mint_format_files}
        COMMENT "Formatting mint C++ sources"
        VERBATIM
    )
    add_custom_target(mint-format-check
        COMMAND "${MINT_CLANG_FORMAT_EXECUTABLE}" --dry-run --Werror ${mint_format_files}
        COMMENT "Checking mint C++ source formatting"
        VERBATIM
    )

    if(PROJECT_IS_TOP_LEVEL)
        add_custom_target(format DEPENDS mint-format)
        add_custom_target(format-check DEPENDS mint-format-check)
    endif()
endfunction()
