add_library(mint_domain STATIC
    change_journal.cpp
    runtime_settings.cpp
    task_policy.cpp
)
add_library(mint::domain ALIAS mint_domain)

mint_configure_target(mint_domain)
target_link_libraries(mint_domain
    PUBLIC nlohmann_json::nlohmann_json
    PRIVATE mint::localization
)
