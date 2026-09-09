include(MintLocalization)

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/locales/en.json"
    "${PROJECT_SOURCE_DIR}/locales/zh-CN.json"
)

mint_embed_catalog("${PROJECT_SOURCE_DIR}/locales/en.json"
    MINT_ENGLISH_CATALOG_PARTS MINT_ENGLISH_CATALOG_PART_COUNT)
mint_embed_catalog("${PROJECT_SOURCE_DIR}/locales/zh-CN.json"
    MINT_SIMPLIFIED_CHINESE_CATALOG_PARTS MINT_SIMPLIFIED_CHINESE_CATALOG_PART_COUNT)
configure_file(catalogs.hpp.in "${CMAKE_CURRENT_BINARY_DIR}/catalogs.hpp" @ONLY)
set(localization_include_dir "${CMAKE_CURRENT_BINARY_DIR}/include")
file(MAKE_DIRECTORY "${localization_include_dir}/mint/localization")
mint_generate_message_ids(
    "${PROJECT_SOURCE_DIR}/locales/en.json"
    "${PROJECT_SOURCE_DIR}/locales/zh-CN.json"
    "${CMAKE_CURRENT_SOURCE_DIR}/message_ids.hpp.in"
    "${localization_include_dir}/mint/localization/message_ids.hpp"
)

add_library(mint_localization STATIC localization.cpp)
add_library(mint::localization ALIAS mint_localization)

mint_configure_target(mint_localization)
target_include_directories(mint_localization
    PUBLIC "${localization_include_dir}"
    PRIVATE "${CMAKE_CURRENT_BINARY_DIR}"
)
target_link_libraries(mint_localization PRIVATE nlohmann_json::nlohmann_json)
