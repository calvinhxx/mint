include_guard(GLOBAL)

find_program(MINT_CLANG_FORMAT_EXECUTABLE NAMES clang-format)
if(MINT_CLANG_FORMAT_EXECUTABLE)
    file(GLOB_RECURSE MINT_FORMAT_FILES CONFIGURE_DEPENDS
        "${PROJECT_SOURCE_DIR}/include/*.hpp"
        "${PROJECT_SOURCE_DIR}/src/*.cpp"
        "${PROJECT_SOURCE_DIR}/src/*.hpp"
        "${PROJECT_SOURCE_DIR}/tests/*.cpp"
        "${PROJECT_SOURCE_DIR}/tests/*.hpp"
    )
    add_custom_target(format
        COMMAND ${MINT_CLANG_FORMAT_EXECUTABLE} -i ${MINT_FORMAT_FILES}
        COMMENT "Formatting C++ sources"
        VERBATIM
    )
    add_custom_target(format-check
        COMMAND ${MINT_CLANG_FORMAT_EXECUTABLE} --dry-run --Werror ${MINT_FORMAT_FILES}
        COMMENT "Checking C++ source formatting"
        VERBATIM
    )
endif()
