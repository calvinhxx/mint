include_guard(GLOBAL)

find_program(AIAGENT_CLANG_FORMAT_EXECUTABLE NAMES clang-format)
if(AIAGENT_CLANG_FORMAT_EXECUTABLE)
    file(GLOB_RECURSE AIAGENT_FORMAT_FILES CONFIGURE_DEPENDS
        "${PROJECT_SOURCE_DIR}/include/*.hpp"
        "${PROJECT_SOURCE_DIR}/src/*.cpp"
        "${PROJECT_SOURCE_DIR}/tests/*.cpp"
        "${PROJECT_SOURCE_DIR}/tests/*.hpp"
    )
    add_custom_target(format
        COMMAND ${AIAGENT_CLANG_FORMAT_EXECUTABLE} -i ${AIAGENT_FORMAT_FILES}
        COMMENT "Formatting C++ sources"
        VERBATIM
    )
    add_custom_target(format-check
        COMMAND ${AIAGENT_CLANG_FORMAT_EXECUTABLE} --dry-run --Werror ${AIAGENT_FORMAT_FILES}
        COMMENT "Checking C++ source formatting"
        VERBATIM
    )
endif()
