include_guard(GLOBAL)

option(AIAGENT_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(AIAGENT_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

add_library(aiagent_project_options INTERFACE)
target_compile_features(aiagent_project_options INTERFACE cxx_std_20)
target_include_directories(aiagent_project_options INTERFACE
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
)

add_library(aiagent_project_warnings INTERFACE)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(aiagent_project_warnings INTERFACE
        -Wall
        -Wextra
        -Wpedantic
        $<$<BOOL:${AIAGENT_WARNINGS_AS_ERRORS}>:-Werror>
    )
elseif(MSVC)
    target_compile_options(aiagent_project_warnings INTERFACE
        /W4
        $<$<BOOL:${AIAGENT_WARNINGS_AS_ERRORS}>:/WX>
    )
endif()

add_library(aiagent_sanitizers INTERFACE)
if(AIAGENT_ENABLE_SANITIZERS)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(aiagent_sanitizers INTERFACE
            -fsanitize=address,undefined
            -fno-omit-frame-pointer
        )
        target_link_options(aiagent_sanitizers INTERFACE
            -fsanitize=address,undefined
        )
    else()
        message(FATAL_ERROR "AIAGENT_ENABLE_SANITIZERS requires Clang or GCC")
    endif()
endif()

function(aiagent_configure_target target)
    target_link_libraries(${target}
        PUBLIC aiagent_project_options
        PRIVATE aiagent_project_warnings aiagent_sanitizers
    )
endfunction()
