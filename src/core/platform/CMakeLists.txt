if(WIN32)
    set(mint_platform_file_source file_windows.cpp)
else()
    set(mint_platform_file_source file_posix.cpp)
endif()

add_library(mint_platform STATIC
    ${mint_platform_file_source}
    path.cpp
    system.cpp
)
add_library(mint::platform ALIAS mint_platform)

mint_configure_target(mint_platform)
