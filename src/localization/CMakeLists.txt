function(mint_embed_catalog input_path output_parts output_count)
    file(READ "${input_path}" catalog)
    set(parts)
    set(count 0)
    while(NOT catalog STREQUAL "")
        string(LENGTH "${catalog}" remaining)
        if(remaining GREATER 24000)
            string(SUBSTRING "${catalog}" 0 24000 prefix)
            string(FIND "${prefix}" "\n" split REVERSE)
            if(split EQUAL -1)
                set(split 24000)
            else()
                math(EXPR split "${split} + 1")
            endif()
        else()
            set(split "${remaining}")
        endif()
        string(SUBSTRING "${catalog}" 0 "${split}" part)
        string(SUBSTRING "${catalog}" "${split}" -1 catalog)
        string(APPEND parts "    R\"mint_locale(${part})mint_locale\",\n")
        math(EXPR count "${count} + 1")
    endwhile()
    set("${output_parts}" "${parts}" PARENT_SCOPE)
    set("${output_count}" "${count}" PARENT_SCOPE)
endfunction()

function(mint_generate_message_ids english_path chinese_path output_path)
    file(READ "${english_path}" english_catalog)
    file(READ "${chinese_path}" chinese_catalog)
    string(JSON message_count LENGTH "${english_catalog}" messages)
    if(message_count LESS 1)
        message(FATAL_ERROR "The localization catalog must not be empty")
    endif()

    set(message_enumerators)
    set(message_keys)
    set(placeholders)
    set(message_identifiers)
    math(EXPR last_message "${message_count} - 1")
    foreach(index RANGE 0 ${last_message})
        string(JSON key MEMBER "${english_catalog}" messages ${index})
        string(JSON english_text GET "${english_catalog}" messages "${key}")
        string(JSON chinese_text GET "${chinese_catalog}" messages "${key}")
        if(NOT key MATCHES "^[a-z][a-z0-9_.-]*$")
            message(FATAL_ERROR "Invalid localization key: ${key}")
        endif()
        string(REGEX REPLACE "[^a-zA-Z0-9_]" "_" identifier "${key}")
        if(identifier IN_LIST message_identifiers)
            message(FATAL_ERROR "Localization keys map to the same C++ identifier: ${key}")
        endif()
        list(APPEND message_identifiers "${identifier}")
        string(REPLACE "*/" "* /" english_comment "${english_text}")
        string(REPLACE "*/" "* /" chinese_comment "${chinese_text}")
        string(REPLACE "\r\n" "\n" english_comment "${english_comment}")
        string(REPLACE "\r\n" "\n" chinese_comment "${chinese_comment}")
        string(REPLACE "\r" "\n" english_comment "${english_comment}")
        string(REPLACE "\r" "\n" chinese_comment "${chinese_comment}")
        string(REPLACE "\n" "\n     *     " english_comment "${english_comment}")
        string(REPLACE "\n" "\n     *     " chinese_comment "${chinese_comment}")
        string(APPEND message_enumerators
            "    /**\n"
            "     * key: ${key}\n"
            "     * EN: ${english_comment}\n"
            "     * ZH-CN: ${chinese_comment}\n"
            "     */\n"
            "    ${identifier},\n"
        )
        string(APPEND message_keys "    \"${key}\",\n")

        string(REGEX MATCHALL "\\{[a-z][a-z0-9_]*\\}" fields "${english_text}")
        foreach(field IN LISTS fields)
            string(LENGTH "${field}" field_length)
            math(EXPR name_length "${field_length} - 2")
            string(SUBSTRING "${field}" 1 ${name_length} name)
            list(APPEND placeholders "${name}")
        endforeach()
    endforeach()

    list(REMOVE_DUPLICATES placeholders)
    list(SORT placeholders)
    set(placeholder_enumerators)
    set(placeholder_names)
    foreach(name IN LISTS placeholders)
        string(APPEND placeholder_enumerators "    ${name},\n")
        string(APPEND placeholder_names "    \"${name}\",\n")
    endforeach()
    list(LENGTH placeholders placeholder_count)

    set(MINT_MESSAGE_ENUMERATORS "${message_enumerators}")
    set(MINT_MESSAGE_KEYS "${message_keys}")
    set(MINT_MESSAGE_COUNT "${message_count}")
    set(MINT_PLACEHOLDER_ENUMERATORS "${placeholder_enumerators}")
    set(MINT_PLACEHOLDER_NAMES "${placeholder_names}")
    set(MINT_PLACEHOLDER_COUNT "${placeholder_count}")
    configure_file(message_ids.hpp.in "${output_path}" @ONLY)
endfunction()

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
