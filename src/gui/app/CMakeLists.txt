add_executable(mint_gui
    gui_main.cpp
    gui_options.cpp
)

set(mint_gui_provider_directory "${PROJECT_BINARY_DIR}/share/mint/providers")
file(GLOB mint_gui_provider_profiles CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/configs/providers/*.json"
)
file(REMOVE_RECURSE "${mint_gui_provider_directory}")
file(MAKE_DIRECTORY "${mint_gui_provider_directory}")
foreach(profile IN LISTS mint_gui_provider_profiles)
    get_filename_component(profile_name "${profile}" NAME)
    configure_file("${profile}" "${mint_gui_provider_directory}/${profile_name}" COPYONLY)
endforeach()

set_target_properties(mint_gui PROPERTIES
    AUTOMOC ON
    OUTPUT_NAME "mint-gui"
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}"
)
mint_configure_target(mint_gui)
target_link_libraries(mint_gui PRIVATE
    mint::gui_cli_adapter
    mint::gui_settings
    mint::gui_ui
    FluentQt::FluentQt
    Qt6::Widgets
)
if(TARGET mint)
    add_dependencies(mint_gui mint)
endif()

if(APPLE AND TARGET mint)
    include(MintMacOSDevelopmentApp)
    mint_add_macos_development_app(mint_gui mint)
endif()
