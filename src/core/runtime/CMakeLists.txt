add_library(mint_runtime STATIC
    path.cpp
    task_control.cpp
    terminal_text.cpp
)
add_library(mint::runtime ALIAS mint_runtime)

mint_configure_target(mint_runtime)
target_link_libraries(mint_runtime PUBLIC mint::ports)
