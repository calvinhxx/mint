include_guard(GLOBAL)

function(mint_enable_sanitizers target)
    if(NOT MINT_ENABLE_SANITIZERS)
        return()
    endif()

    if(NOT TARGET "${target}")
        message(FATAL_ERROR "Unknown mint target: ${target}")
    endif()

    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options("${target}" PRIVATE
            -fsanitize=address,undefined
            -fno-sanitize-recover=undefined
            -fno-omit-frame-pointer
        )
        target_link_options("${target}" PRIVATE
            -fsanitize=address,undefined
            -fno-sanitize-recover=undefined
        )
    else()
        message(FATAL_ERROR
            "MINT_ENABLE_SANITIZERS requires GCC or Clang "
            "(compiler: ${CMAKE_CXX_COMPILER_ID})"
        )
    endif()
endfunction()
