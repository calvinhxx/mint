if(NOT DEFINED SOURCE_DIR)
    message(FATAL_ERROR "SOURCE_DIR is required")
endif()

file(GLOB_RECURSE production_sources
    LIST_DIRECTORIES FALSE
    "${SOURCE_DIR}/include/*.hpp"
    "${SOURCE_DIR}/include/*.h"
    "${SOURCE_DIR}/src/*.cpp"
    "${SOURCE_DIR}/src/*.hpp"
    "${SOURCE_DIR}/src/*.h"
    "${SOURCE_DIR}/src/*.mm"
)

set(platform_macro
    "(_WIN32|_WIN64|WIN32|WIN64|__APPLE__|__MACH__|__linux__|__linux|linux|__unix__|__unix|unix|__FreeBSD__|__NetBSD__|__OpenBSD__|__DragonFly__|__CYGWIN__|__MINGW32__|__MINGW64__|Q_OS_[A-Z0-9_]+)"
)
set(native_header
    "(windows\\.h|winsock2?\\.h|ws2tcpip\\.h|unistd\\.h|process\\.h|io\\.h|fcntl\\.h|strings\\.h|sys/[^>\"]+|mach/[^>\"]+|mach-o/[^>\"]+|aclapi\\.h|sddl\\.h|userenv\\.h|winternl\\.h|shellapi\\.h|shlobj\\.h|libproc\\.h|spawn\\.h)"
)
set(violations)
foreach(source IN LISTS production_sources)
    file(RELATIVE_PATH relative "${SOURCE_DIR}" "${source}")
    if(relative MATCHES "^src/(core/platform/[^/]+\\.cpp|cli/platform/[^/]+\\.cpp)$" OR
       relative MATCHES "^src/core/infrastructure/(command|filesystem)/native/")
        continue()
    endif()
    file(READ "${source}" contents)
    string(REPLACE "\r\n" "\n" contents "${contents}")
    string(REPLACE "\\\n" "" contents "${contents}")
    string(REGEX MATCHALL "(^|\n)[ \t]*#[ \t]*(if|ifdef|ifndef|elif)([^A-Za-z0-9_][^\n]*)"
        conditionals "${contents}")
    foreach(conditional IN LISTS conditionals)
        if(conditional MATCHES "(^|[^A-Za-z0-9_])${platform_macro}([^A-Za-z0-9_]|$)")
            list(APPEND violations "${relative}: platform conditional")
        endif()
    endforeach()
    string(TOLOWER "${contents}" lower_contents)
    if(lower_contents MATCHES "(^|\n)[ \t]*#[ \t]*include[ \t]*[<\"]${native_header}[>\"]")
        list(APPEND violations "${relative}: native system header")
    endif()
endforeach()

if(violations)
    list(REMOVE_DUPLICATES violations)
    list(JOIN violations ", " violation_list)
    message(FATAL_ERROR
        "Keep platform details in platform implementations or native backends: ${violation_list}"
    )
endif()
