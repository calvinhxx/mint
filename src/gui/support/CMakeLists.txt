add_library(mint_gui_support STATIC qt_path.cpp)
add_library(mint::gui_support ALIAS mint_gui_support)

mint_configure_target(mint_gui_support)
target_include_directories(mint_gui_support PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src/gui>
)
target_link_libraries(mint_gui_support
    PUBLIC Qt6::Core
    PRIVATE mint::platform
)
