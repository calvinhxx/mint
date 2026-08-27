add_library(mint_runtime STATIC
    task_control.cpp
)
add_library(mint::runtime ALIAS mint_runtime)

mint_configure_target(mint_runtime)
