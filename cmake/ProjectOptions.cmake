include_guard(GLOBAL)

option(MINT_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(MINT_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

add_library(mint_project_options INTERFACE)
target_compile_features(mint_project_options INTERFACE cxx_std_20)
target_include_directories(mint_project_options INTERFACE
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
)

add_library(mint_project_warnings INTERFACE)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(mint_project_warnings INTERFACE
        -Wall
        -Wextra
        -Wpedantic
        $<$<BOOL:${MINT_WARNINGS_AS_ERRORS}>:-Werror>
    )
elseif(MSVC)
    target_compile_options(mint_project_warnings INTERFACE
        /W4
        $<$<BOOL:${MINT_WARNINGS_AS_ERRORS}>:/WX>
    )
endif()

add_library(mint_sanitizers INTERFACE)
if(MINT_ENABLE_SANITIZERS)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(mint_sanitizers INTERFACE
            -fsanitize=address,undefined
            -fno-omit-frame-pointer
        )
        target_link_options(mint_sanitizers INTERFACE
            -fsanitize=address,undefined
        )
    else()
        message(FATAL_ERROR "MINT_ENABLE_SANITIZERS requires Clang or GCC")
    endif()
endif()

function(mint_configure_target target)
    target_link_libraries(${target}
        PUBLIC mint_project_options
        PRIVATE mint_project_warnings mint_sanitizers
    )
endfunction()
