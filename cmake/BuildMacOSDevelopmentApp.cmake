cmake_minimum_required(VERSION 3.24)

foreach(input MINT_GUI_EXECUTABLE MINT_CLI_EXECUTABLE MINT_APP_INFO_PLIST MINT_CODESIGN_EXECUTABLE)
    if(NOT DEFINED ${input} OR NOT IS_ABSOLUTE "${${input}}" OR
       NOT EXISTS "${${input}}" OR IS_DIRECTORY "${${input}}" OR IS_SYMLINK "${${input}}")
        message(FATAL_ERROR "${input} must identify an existing regular build input")
    endif()
endforeach()
get_filename_component(bundle_name "${MINT_APP_BUNDLE}" NAME)
if(NOT IS_ABSOLUTE "${MINT_APP_BUNDLE}" OR NOT bundle_name STREQUAL "Mint.app")
    message(FATAL_ERROR "The development app destination must be an absolute Mint.app path")
endif()

function(check_bundle_destination)
    foreach(relative "" "/Contents" "/Contents/MacOS" "/Contents/Info.plist"
                     "/Contents/MacOS/mint-gui" "/Contents/MacOS/mint")
        if(IS_SYMLINK "${MINT_APP_BUNDLE}${relative}")
            message(FATAL_ERROR "The development app destination contains a symlink")
        endif()
    endforeach()
    if(EXISTS "${MINT_APP_BUNDLE}" AND NOT IS_DIRECTORY "${MINT_APP_BUNDLE}")
        message(FATAL_ERROR "The development app destination is not a directory")
    endif()
endfunction()

check_bundle_destination()
get_filename_component(bundle_parent "${MINT_APP_BUNDLE}" DIRECTORY)
string(RANDOM LENGTH 16 ALPHABET 0123456789abcdef temporary_id)
set(staging_bundle "${bundle_parent}/.Mint-${temporary_id}.app")
if(EXISTS "${staging_bundle}" OR IS_SYMLINK "${staging_bundle}")
    message(FATAL_ERROR "The development app staging path already exists")
endif()
file(MAKE_DIRECTORY "${staging_bundle}/Contents/MacOS")
file(COPY_FILE "${MINT_GUI_EXECUTABLE}" "${staging_bundle}/Contents/MacOS/mint-gui")
file(COPY_FILE "${MINT_CLI_EXECUTABLE}" "${staging_bundle}/Contents/MacOS/mint")
file(COPY_FILE "${MINT_APP_INFO_PLIST}" "${staging_bundle}/Contents/Info.plist")
file(CHMOD "${staging_bundle}/Contents/MacOS/mint-gui" "${staging_bundle}/Contents/MacOS/mint"
    PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE
)

function(sign_or_verify)
    execute_process(
        COMMAND "${MINT_CODESIGN_EXECUTABLE}" ${ARGN}
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error
    )
    if(NOT result STREQUAL "0")
        file(REMOVE_RECURSE "${staging_bundle}")
        message(FATAL_ERROR "Development app signing failed: ${error}${output}")
    endif()
endfunction()

# EN: Sign copied nested code before the bundle; never sign the bare programs or external Qt SDK.
# ZH-CN: 先签名复制后的内嵌程序，再签名 bundle；不签名裸程序或外部 Qt SDK。
sign_or_verify(--force --sign - --timestamp=none "${staging_bundle}/Contents/MacOS/mint")
sign_or_verify(--force --sign - --timestamp=none "${staging_bundle}")
sign_or_verify(--verify --strict "${staging_bundle}/Contents/MacOS/mint")
sign_or_verify(--verify --strict "${staging_bundle}")

# EN: Replace only this generated app after signing succeeds; existing contents are never signed.
# ZH-CN: 签名成功后只替换这个生成的 App，绝不对已有目录内容执行签名。
check_bundle_destination()
file(REMOVE_RECURSE "${MINT_APP_BUNDLE}")
file(RENAME "${staging_bundle}" "${MINT_APP_BUNDLE}" RESULT rename_result)
if(NOT rename_result STREQUAL "0")
    message(FATAL_ERROR "Cannot publish the development app: ${rename_result}")
endif()
message(STATUS "Created ${MINT_APP_BUNDLE} (uses the local Qt SDK)")
