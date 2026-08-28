cmake_minimum_required(VERSION 3.24)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    include(cmake/MintPlatformDefaults.cmake)
    if(CMAKE_HOST_APPLE)
        set(MINT_MACOS_DEPLOYMENT_TARGET "${MINT_MACOS_DEPLOYMENT_TARGET}"
            CACHE STRING "Minimum supported macOS version"
        )
        set(CMAKE_OSX_DEPLOYMENT_TARGET "${MINT_MACOS_DEPLOYMENT_TARGET}"
            CACHE STRING "Minimum supported macOS version" FORCE
        )
    endif()
    list(PREPEND VCPKG_OVERLAY_TRIPLETS "${CMAKE_CURRENT_LIST_DIR}/cmake/triplets")
    list(REMOVE_DUPLICATES VCPKG_OVERLAY_TRIPLETS)
    set(VCPKG_OVERLAY_TRIPLETS "${VCPKG_OVERLAY_TRIPLETS}"
        CACHE STRING "Additional vcpkg triplet directories" FORCE
    )
endif()

project(mint VERSION 1.5.0 DESCRIPTION "Lightweight general AI agent toolkit" LANGUAGES CXX)

list(PREPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

option(BUILD_TESTING "Enable testing" ${PROJECT_IS_TOP_LEVEL})
option(MINT_BUILD_TESTS "Build mint test targets" ${PROJECT_IS_TOP_LEVEL})
option(MINT_BUILD_CLI "Build the mint command-line application" ${PROJECT_IS_TOP_LEVEL})
option(MINT_ENABLE_PACKAGING "Add install rules and archive packages" ${MINT_BUILD_CLI})
option(MINT_ENABLE_DEVELOPER_TOOLS "Add source formatting targets" ${PROJECT_IS_TOP_LEVEL})
option(MINT_ENABLE_WARNINGS "Enable strict compiler warnings" ${PROJECT_IS_TOP_LEVEL})
option(MINT_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(MINT_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

include(MintTargetOptions)

find_package(CURL REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)

add_subdirectory(src)

if(MINT_ENABLE_DEVELOPER_TOOLS)
    include(MintDeveloperTools)
    mint_add_developer_tools()
endif()

include(CTest)
if(BUILD_TESTING AND MINT_BUILD_TESTS)
    if(NOT TARGET mint)
        message(FATAL_ERROR "MINT_BUILD_TESTS requires MINT_BUILD_CLI")
    endif()
    add_subdirectory(tests)
endif()

if(MINT_ENABLE_PACKAGING)
    if(NOT TARGET mint)
        message(FATAL_ERROR "MINT_ENABLE_PACKAGING requires MINT_BUILD_CLI")
    endif()
    include(MintPackaging)
    mint_configure_packaging(mint)
endif()
