add_library(mint_application STATIC
    agent.cpp
    agent_checkpoint.cpp
    agent_context.cpp
    agent_cycle.cpp
    agent_loop.cpp
    agent_prompt.cpp
    agent_state.cpp
    project_service.cpp
)
add_library(mint::application ALIAS mint_application)

mint_configure_target(mint_application)
target_link_libraries(mint_application
    PUBLIC
        mint::domain
        mint::infrastructure
        mint::runtime
        mint::tools
)
