include_guard(GLOBAL)

set(MINT_FLUENTQT_SOURCE_DIR "" CACHE PATH "Optional FluentQt 1.8.1+ source directory for the GUI")
option(MINT_USE_SYSTEM_FLUENTQT "Use an installed FluentQt SDK instead of fetching source" OFF)

function(mint_configure_gui_dependencies)
    if(MINT_USE_SYSTEM_FLUENTQT)
        if(MINT_FLUENTQT_SOURCE_DIR)
            message(FATAL_ERROR
                "Choose either MINT_USE_SYSTEM_FLUENTQT or MINT_FLUENTQT_SOURCE_DIR, not both.")
        endif()
        find_package(FluentQt 1.8.1 CONFIG REQUIRED)
        message(STATUS "Mint GUI: installed FluentQt ${FluentQt_VERSION} at ${FluentQt_DIR}")
        return()
    endif()

    # EN: Keep dependency options local; Mint still owns BUILD_TESTING and its toolchain.
    # ZH-CN: 依赖选项仅在局部生效；BUILD_TESTING 和工具链仍由 Mint 管理。
    set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
    set(FLUENT_QT_BUILD_TESTS OFF)
    set(FLUENT_QT_BUILD_GALLERY OFF)
    set(FLUENT_QT_BUILD_EXAMPLES OFF)
    set(FLUENT_QT_BUILD_PYSIDE6_BINDINGS OFF)
    set(FLUENT_QT_BUILD_WEBASSEMBLY_SUPPORT OFF)
    set(FLUENT_QT_INSTALL OFF)
    set(FLUENT_QT_ENABLE_GALLERY_PACKAGING OFF)
    set(QT_DEFAULT_MAJOR_VERSION 6)
    set(QT_DIR "${Qt6_DIR}")

    if(MINT_FLUENTQT_SOURCE_DIR)
        get_filename_component(fluentqt_source "${MINT_FLUENTQT_SOURCE_DIR}" ABSOLUTE
            BASE_DIR "${PROJECT_SOURCE_DIR}")
        if(NOT EXISTS "${fluentqt_source}/CMakeLists.txt")
            message(FATAL_ERROR
                "MINT_FLUENTQT_SOURCE_DIR must point to a FluentQt source checkout. "
                "Leave it empty to fetch FluentQt automatically.")
        endif()
        set(fluentqt_binary "${PROJECT_BINARY_DIR}/_deps/fluentqt")
        add_subdirectory("${fluentqt_source}" "${fluentqt_binary}" EXCLUDE_FROM_ALL)
    else()
        include(FetchContent)
        # EN: Only the explicit SDK mode may reuse an installed FluentQt package.
        # ZH-CN: 只有显式 SDK 模式才允许复用已安装的 FluentQt 包。
        set(FETCHCONTENT_TRY_FIND_PACKAGE_MODE NEVER)
        FetchContent_Declare(fluentqt
            GIT_REPOSITORY https://github.com/calvinhxx/Fluent-Qt.git
            GIT_TAG v1.8.1
            GIT_SHALLOW TRUE
        )
        FetchContent_MakeAvailable(fluentqt)
        set(fluentqt_source "${fluentqt_SOURCE_DIR}")
        set(fluentqt_binary "${fluentqt_BINARY_DIR}")
    endif()
    get_directory_property(fluentqt_version DIRECTORY "${fluentqt_binary}"
        DEFINITION FluentQt_VERSION)
    if(NOT TARGET FluentQt::FluentQt OR NOT fluentqt_version OR
       fluentqt_version VERSION_LESS 1.8.1)
        message(FATAL_ERROR "The GUI requires FluentQt 1.8.1 or newer source code.")
    endif()

    # EN: Imported SDK headers are system headers; keep the same boundary for source builds.
    # ZH-CN: 已安装 SDK 的头文件属于系统头；源码构建保持同样的警告边界。
    get_target_property(fluentqt_target FluentQt::FluentQt ALIASED_TARGET)
    get_target_property(fluentqt_includes "${fluentqt_target}" INTERFACE_INCLUDE_DIRECTORIES)
    set_property(TARGET "${fluentqt_target}" APPEND PROPERTY
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${fluentqt_includes}")
    message(STATUS "Mint GUI: FluentQt ${fluentqt_version} source at ${fluentqt_source}")
endfunction()
