add_library(mint_ports INTERFACE)
add_library(mint::ports ALIAS mint_ports)

target_compile_features(mint_ports INTERFACE cxx_std_20)
target_include_directories(mint_ports INTERFACE
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
)
target_link_libraries(mint_ports INTERFACE mint::domain)
