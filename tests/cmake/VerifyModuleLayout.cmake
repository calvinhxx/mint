if(NOT DEFINED SOURCE_DIR)
    message(FATAL_ERROR "SOURCE_DIR is required")
endif()

set(required_directories
    src/application/agent
    src/cli/agent
    src/cli/project
    src/cli/provider
    src/cli/support
    src/infrastructure/command
    src/infrastructure/filesystem
    src/infrastructure/logging
    src/infrastructure/model
    src/infrastructure/persistence
    src/tools/changes
    src/tools/registry
    src/tools/workspace
)
foreach(directory IN LISTS required_directories)
    if(NOT IS_DIRECTORY "${SOURCE_DIR}/${directory}")
        message(FATAL_ERROR "Missing module directory: ${directory}")
    endif()
endforeach()

file(GLOB ungrouped_cli_sources
    LIST_DIRECTORIES FALSE
    "${SOURCE_DIR}/src/cli/*.cpp"
    "${SOURCE_DIR}/src/cli/*.hpp"
)
list(REMOVE_ITEM ungrouped_cli_sources
    "${SOURCE_DIR}/src/cli/command_line.cpp"
    "${SOURCE_DIR}/src/cli/command_line.hpp"
    "${SOURCE_DIR}/src/cli/main.cpp"
)
if(ungrouped_cli_sources)
    message(FATAL_ERROR
        "CLI feature files must belong to a feature module: ${ungrouped_cli_sources}"
    )
endif()

file(GLOB ungrouped_tool_sources
    LIST_DIRECTORIES FALSE
    "${SOURCE_DIR}/src/tools/*.cpp"
    "${SOURCE_DIR}/src/tools/*.hpp"
)
if(ungrouped_tool_sources)
    message(FATAL_ERROR
        "Tool implementation files must belong to a feature module: ${ungrouped_tool_sources}"
    )
endif()

file(GLOB ungrouped_infrastructure_sources
    LIST_DIRECTORIES FALSE
    "${SOURCE_DIR}/src/infrastructure/*.cpp"
    "${SOURCE_DIR}/src/infrastructure/*.hpp"
)
if(ungrouped_infrastructure_sources)
    message(FATAL_ERROR
        "Infrastructure implementation files must belong to a feature module: "
        "${ungrouped_infrastructure_sources}"
    )
endif()

file(GLOB ungrouped_agent_sources
    LIST_DIRECTORIES FALSE
    "${SOURCE_DIR}/src/application/agent*.cpp"
    "${SOURCE_DIR}/src/application/agent*.hpp"
)
if(ungrouped_agent_sources)
    message(FATAL_ERROR
        "Agent implementation files must stay in src/application/agent: "
        "${ungrouped_agent_sources}"
    )
endif()

file(READ "${SOURCE_DIR}/src/application/CMakeLists.txt" application_cmake)
foreach(target IN ITEMS mint_agent mint_project)
    string(REGEX MATCH "add_library\\(${target}[ \t\r\n]+STATIC" target_definition
                 "${application_cmake}")
    if(NOT target_definition)
        message(FATAL_ERROR "Application module target is missing: ${target}")
    endif()
endforeach()

file(READ "${SOURCE_DIR}/src/infrastructure/CMakeLists.txt" infrastructure_cmake)
foreach(target IN ITEMS mint_command mint_filesystem mint_logging mint_model mint_persistence)
    string(REGEX MATCH "add_library\\(${target}[ \t\r\n]+STATIC" target_definition
                 "${infrastructure_cmake}")
    if(NOT target_definition)
        message(FATAL_ERROR "Infrastructure module target is missing: ${target}")
    endif()
endforeach()
foreach(module_cmake IN ITEMS application_cmake infrastructure_cmake)
    string(REGEX MATCH "add_library\\(mint_(application|infrastructure)" facade_definition
                 "${${module_cmake}}")
    if(facade_definition)
        message(FATAL_ERROR "Layer-wide compatibility facades must not be reintroduced")
    endif()
endforeach()

file(READ "${SOURCE_DIR}/src/CMakeLists.txt" root_cmake)
string(REGEX MATCH "mint::(application|infrastructure)" facade_dependency "${root_cmake}")
if(facade_dependency)
    message(FATAL_ERROR "mint_core must aggregate feature targets directly")
endif()

foreach(cmake_file IN ITEMS src/tools/CMakeLists.txt src/cli/CMakeLists.txt)
    file(READ "${SOURCE_DIR}/${cmake_file}" consumer_cmake)
    string(REGEX MATCH "mint::core" umbrella_dependency "${consumer_cmake}")
    if(umbrella_dependency)
        message(FATAL_ERROR
            "${cmake_file} must link the feature modules it uses, not ${umbrella_dependency}"
        )
    endif()
endforeach()
