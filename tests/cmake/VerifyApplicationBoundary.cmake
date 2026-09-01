if(NOT DEFINED SOURCE_DIR)
    message(FATAL_ERROR "SOURCE_DIR is required")
endif()

cmake_policy(SET CMP0057 NEW)

file(GLOB_RECURSE application_sources
    LIST_DIRECTORIES FALSE
    "${SOURCE_DIR}/include/mint/application/*.hpp"
    "${SOURCE_DIR}/src/application/*.cpp"
    "${SOURCE_DIR}/src/application/*.hpp"
)

set(violations)
foreach(source IN LISTS application_sources)
    file(READ "${source}" contents)
    string(REGEX MATCHALL "#include[ \t]+[<\"]mint/[^>\"\n]+[>\"]" mint_includes
                 "${contents}")
    foreach(include IN LISTS mint_includes)
        string(REGEX REPLACE ".*[<\"]mint/([^>\"]+)[>\"].*" "\\1" include_path
                     "${include}")
        if(NOT include_path MATCHES "^(application|domain|ports|runtime)/" AND
           NOT include_path STREQUAL "version.hpp")
            file(RELATIVE_PATH relative "${SOURCE_DIR}" "${source}")
            list(APPEND violations "${relative}: mint/${include_path}")
        endif()
    endforeach()
endforeach()

file(READ "${SOURCE_DIR}/src/application/CMakeLists.txt" application_cmake)
string(REGEX MATCHALL "target_link_libraries\\(mint_(agent|project)[^\\)]*\\)"
             link_blocks "${application_cmake}")
list(LENGTH link_blocks link_block_count)
if(link_block_count LESS 2)
    list(APPEND violations "src/application/CMakeLists.txt: missing dependency declarations")
endif()

set(allowed_targets
    mint_agent
    mint_project
    mint::domain
    mint::ports
    mint::runtime
)
foreach(block IN LISTS link_blocks)
    string(REGEX MATCHALL "mint(::|_)[A-Za-z0-9_]+" linked_targets "${block}")
    foreach(target IN LISTS linked_targets)
        if(NOT target IN_LIST allowed_targets)
            list(APPEND violations "src/application/CMakeLists.txt: ${target}")
        endif()
    endforeach()
endforeach()

if(violations)
    list(REMOVE_DUPLICATES violations)
    list(JOIN violations ", " violation_list)
    message(FATAL_ERROR
        "Application may depend only on domain, ports, and runtime: ${violation_list}"
    )
endif()
