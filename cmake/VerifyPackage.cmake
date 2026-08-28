if(NOT DEFINED MINT_BUILD_DIR)
    message(FATAL_ERROR "MINT_BUILD_DIR is required")
endif()

get_filename_component(MINT_BUILD_DIR "${MINT_BUILD_DIR}" ABSOLUTE)
set(cpack_config "${MINT_BUILD_DIR}/CPackConfig.cmake")
if(NOT EXISTS "${cpack_config}")
    message(FATAL_ERROR "CPack configuration not found: ${cpack_config}")
endif()
include("${cpack_config}")

if(CPACK_GENERATOR STREQUAL "ZIP")
    set(package_extension zip)
elseif(CPACK_GENERATOR STREQUAL "TGZ")
    set(package_extension tar.gz)
else()
    message(FATAL_ERROR "Unsupported mint package generator: ${CPACK_GENERATOR}")
endif()

set(package_path
    "${CPACK_PACKAGE_DIRECTORY}/${CPACK_PACKAGE_FILE_NAME}.${package_extension}"
)
set(checksum_path "${package_path}.sha256")
foreach(required_file IN ITEMS "${package_path}" "${checksum_path}")
    if(NOT EXISTS "${required_file}")
        message(FATAL_ERROR "Expected package output not found: ${required_file}")
    endif()
endforeach()

file(SHA256 "${package_path}" actual_checksum)
file(READ "${checksum_path}" checksum_contents)
string(REGEX MATCH "^[0-9A-Fa-f]+" expected_checksum "${checksum_contents}")
string(TOLOWER "${expected_checksum}" expected_checksum)
if(NOT actual_checksum STREQUAL expected_checksum)
    message(FATAL_ERROR "Package checksum does not match ${checksum_path}")
endif()

set(extract_dir "${MINT_BUILD_DIR}/package-verify")
file(REMOVE_RECURSE "${extract_dir}")
file(MAKE_DIRECTORY "${extract_dir}")
file(ARCHIVE_EXTRACT INPUT "${package_path}" DESTINATION "${extract_dir}")
set(package_root "${extract_dir}/${CPACK_PACKAGE_FILE_NAME}")

if(WIN32)
    set(executable "${package_root}/bin/mint.exe")
else()
    set(executable "${package_root}/bin/mint")
endif()

foreach(required_file IN ITEMS
    "${executable}"
    "${package_root}/share/mint/providers/groq-chat.json"
    "${package_root}/share/mint/providers/openai-responses.json"
    "${package_root}/share/doc/mint/README.md"
    "${package_root}/share/doc/mint/LICENSE"
    "${package_root}/share/licenses/mint/curl/copyright"
)
    if(NOT EXISTS "${required_file}")
        message(FATAL_ERROR "Package is missing: ${required_file}")
    endif()
endforeach()

execute_process(
    COMMAND "${executable}" --version
    RESULT_VARIABLE version_result
    OUTPUT_VARIABLE version_output
    ERROR_VARIABLE version_error
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT version_result EQUAL 0 OR NOT version_output STREQUAL "mint ${CPACK_PACKAGE_VERSION}")
    message(FATAL_ERROR
        "Packaged executable failed version check: result=${version_result}, "
        "stdout='${version_output}', stderr='${version_error}'"
    )
endif()

if(CMAKE_HOST_APPLE AND CPACK_PACKAGE_FILE_NAME MATCHES "-macos-")
    if(NOT DEFINED CPACK_MINT_MACOS_DEPLOYMENT_TARGET)
        message(FATAL_ERROR "Package has no recorded macOS deployment target")
    endif()
    find_program(otool_program otool REQUIRED)
    execute_process(
        COMMAND "${otool_program}" -l "${executable}"
        RESULT_VARIABLE otool_result
        OUTPUT_VARIABLE load_commands
        ERROR_VARIABLE otool_error
    )
    string(REPLACE "." "\\." deployment_pattern "${CPACK_MINT_MACOS_DEPLOYMENT_TARGET}")
    if(NOT otool_result EQUAL 0 OR
       NOT load_commands MATCHES "minos[ \t]+${deployment_pattern}([ \t\r\n]|$)")
        message(FATAL_ERROR
            "Packaged executable has the wrong macOS deployment target: "
            "expected=${CPACK_MINT_MACOS_DEPLOYMENT_TARGET}, stderr='${otool_error}'"
        )
    endif()
endif()

message(STATUS "Verified ${package_path}")
