add_library(mint_test_support STATIC
    command_helper.cpp
    scripted_http_server.cpp
)
mint_configure_target(mint_test_support)
target_include_directories(mint_test_support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

if(WIN32)
    target_link_libraries(mint_test_support PUBLIC ws2_32)
endif()
