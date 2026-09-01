add_library(mint_agent STATIC
    agent/agent.cpp
    agent/agent_checkpoint.cpp
    agent/agent_conversation.cpp
    agent/agent_context.cpp
    agent/agent_cycle.cpp
    agent/agent_execution.cpp
    agent/agent_loop.cpp
    agent/agent_model_summary.cpp
    agent/agent_prompt.cpp
    agent/agent_reporting.cpp
)
add_library(mint::agent ALIAS mint_agent)

mint_configure_target(mint_agent)
target_link_libraries(mint_agent
    PUBLIC
        mint::domain
        mint::ports
        mint::runtime
)

add_library(mint_project STATIC
    project_service.cpp
)
add_library(mint::project ALIAS mint_project)

mint_configure_target(mint_project)
target_link_libraries(mint_project
    PUBLIC
        mint::domain
    PRIVATE
        mint::runtime
)

# Compatibility facade for consumers that want the complete application layer.
add_library(mint_application INTERFACE)
add_library(mint::application ALIAS mint_application)
target_link_libraries(mint_application INTERFACE
    mint::agent
    mint::project
)
