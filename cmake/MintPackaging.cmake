include_guard(GLOBAL)

include(GNUInstallDirs)

function(mint_package_architecture output_variable)
    if(DEFINED VCPKG_TARGET_TRIPLET AND VCPKG_TARGET_TRIPLET MATCHES "^([^-]+)-")
        set(raw_architecture "${CMAKE_MATCH_1}")
    else()
        set(raw_architecture "${CMAKE_SYSTEM_PROCESSOR}")
    endif()

    string(TOLOWER "${raw_architecture}" architecture)
    if(architecture MATCHES "^(amd64|x86_64|x64)$")
        set(architecture x64)
    elseif(architecture MATCHES "^(aarch64|arm64)$")
        set(architecture arm64)
    endif()
    string(REGEX REPLACE "[^a-z0-9_.-]" "-" architecture "${architecture}")

    if(architecture STREQUAL "")
        message(FATAL_ERROR "Unable to determine the package architecture")
    endif()
    set("${output_variable}" "${architecture}" PARENT_SCOPE)
endfunction()

function(mint_install_third_party_licenses)
    if(NOT DEFINED VCPKG_INSTALLED_DIR OR NOT DEFINED VCPKG_TARGET_TRIPLET)
        return()
    endif()

    set(share_dir "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share")
    file(GLOB license_files "${share_dir}/*/copyright")
    foreach(license_file IN LISTS license_files)
        get_filename_component(port_dir "${license_file}" DIRECTORY)
        get_filename_component(port_name "${port_dir}" NAME)
        if(port_name MATCHES "^vcpkg-")
            continue()
        endif()
        install(FILES "${license_file}"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/mint/${port_name}"
        )
    endforeach()
endfunction()

function(mint_configure_packaging target)
    if(NOT PROJECT_IS_TOP_LEVEL)
        message(FATAL_ERROR "mint packages can only be configured by the top-level project")
    endif()
    if(NOT TARGET "${target}")
        message(FATAL_ERROR "Unknown mint package target: ${target}")
    endif()

    if(WIN32)
        set(runtime_dependency_options
            PRE_EXCLUDE_REGEXES "api-ms-.*" "ext-ms-.*"
            POST_EXCLUDE_REGEXES ".*[Ss][Yy][Ss][Tt][Ee][Mm]32.*"
        )
        if(DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
            list(PREPEND runtime_dependency_options DIRECTORIES
                "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin"
            )
        endif()
        install(TARGETS "${target}"
            RUNTIME_DEPENDENCIES ${runtime_dependency_options}
            RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
        )
    else()
        install(TARGETS "${target}"
            RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
        )
    endif()

    install(DIRECTORY "${PROJECT_SOURCE_DIR}/configs/providers/"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/mint/providers"
        FILES_MATCHING PATTERN "*.json"
    )
    install(FILES
        "${PROJECT_SOURCE_DIR}/README.md"
        "${PROJECT_SOURCE_DIR}/CHANGELOG.md"
        "${PROJECT_SOURCE_DIR}/LICENSE"
        DESTINATION "${CMAKE_INSTALL_DOCDIR}"
    )
    mint_install_third_party_licenses()

    if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        set(package_platform macos)
    else()
        string(TOLOWER "${CMAKE_SYSTEM_NAME}" package_platform)
    endif()
    mint_package_architecture(package_architecture)

    set(CPACK_PACKAGE_NAME "mint")
    set(CPACK_PACKAGE_VENDOR "mint contributors")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Lightweight general AI agent tool")
    set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
    set(CPACK_PACKAGE_FILE_NAME
        "mint-${PROJECT_VERSION}-${package_platform}-${package_architecture}"
    )
    set(CPACK_PACKAGE_DIRECTORY "${PROJECT_BINARY_DIR}/packages")
    set(CPACK_PACKAGE_CHECKSUM SHA256)
    set(CPACK_RESOURCE_FILE_LICENSE "${PROJECT_SOURCE_DIR}/LICENSE")
    set(CPACK_MONOLITHIC_INSTALL ON)
    set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
    set(CPACK_VERBATIM_VARIABLES ON)
    if(APPLE)
        set(CPACK_MINT_MACOS_DEPLOYMENT_TARGET "${CMAKE_OSX_DEPLOYMENT_TARGET}")
    endif()
    if(WIN32)
        set(CPACK_GENERATOR ZIP)
    else()
        set(CPACK_GENERATOR TGZ)
    endif()

    include(CPack)
endfunction()
