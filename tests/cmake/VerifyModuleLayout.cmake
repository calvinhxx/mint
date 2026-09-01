if(NOT DEFINED SOURCE_DIR)
    message(FATAL_ERROR "SOURCE_DIR is required")
endif()

set(required_directories
    src/application/agent
    src/infrastructure/command
    src/infrastructure/filesystem
    src/infrastructure/logging
    src/infrastructure/model
    src/infrastructure/persistence
)
foreach(directory IN LISTS required_directories)
    if(NOT IS_DIRECTORY "${SOURCE_DIR}/${directory}")
        message(FATAL_ERROR "Missing module directory: ${directory}")
    endif()
endforeach()

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
string(REGEX MATCH "add_library\\(mint_infrastructure[ \t\r\n]+STATIC" monolith_definition
             "${infrastructure_cmake}")
if(monolith_definition)
    message(FATAL_ERROR "mint_infrastructure must remain an interface facade, not a monolith")
endif()

foreach(cmake_file IN ITEMS src/tools/CMakeLists.txt src/cli/CMakeLists.txt)
    file(READ "${SOURCE_DIR}/${cmake_file}" consumer_cmake)
    string(REGEX MATCH "mint::(core|infrastructure)" umbrella_dependency "${consumer_cmake}")
    if(umbrella_dependency)
        message(FATAL_ERROR
            "${cmake_file} must link the feature modules it uses, not ${umbrella_dependency}"
        )
    endif()
endforeach()
