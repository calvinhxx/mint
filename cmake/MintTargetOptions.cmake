include_guard(GLOBAL)

include(MintSanitizers)

function(mint_configure_cpp_target target)
    if(NOT TARGET "${target}")
        message(FATAL_ERROR "Unknown mint target: ${target}")
    endif()

    get_target_property(target_type "${target}" TYPE)
    if(target_type STREQUAL "EXECUTABLE")
        set(usage_requirement PRIVATE)
    else()
        set(usage_requirement PUBLIC)
    endif()

    target_compile_features("${target}" ${usage_requirement} cxx_std_20)
    target_include_directories("${target}" ${usage_requirement}
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    )
    set_target_properties("${target}" PROPERTIES
        CXX_EXTENSIONS OFF
        CXX_STANDARD_REQUIRED ON
    )

    if(MSVC)
        target_compile_options("${target}" PRIVATE /utf-8)
    endif()
endfunction()

function(mint_enable_project_warnings target)
    if(NOT MINT_ENABLE_WARNINGS)
        return()
    endif()

    if(NOT TARGET "${target}")
        message(FATAL_ERROR "Unknown mint target: ${target}")
    endif()

    if(MSVC)
        target_compile_options("${target}" PRIVATE
            /W4
            $<$<BOOL:${MINT_WARNINGS_AS_ERRORS}>:/WX>
        )
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options("${target}" PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            $<$<BOOL:${MINT_WARNINGS_AS_ERRORS}>:-Werror>
        )
    endif()
endfunction()

function(mint_configure_target target)
    mint_configure_cpp_target("${target}")
    mint_enable_project_warnings("${target}")
    mint_enable_sanitizers("${target}")
endfunction()
