include_guard(GLOBAL)

include(GoogleTest)

function(mint_add_gtest target)
    set(options GTEST_MAIN)
    set(one_value_args TEST_PREFIX)
    set(multi_value_args SOURCES LIBRARIES INCLUDE_DIRECTORIES LABELS EXTRA_ARGS)
    cmake_parse_arguments(PARSE_ARGV 1 test
        "${options}"
        "${one_value_args}"
        "${multi_value_args}"
    )

    if(test_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "mint_add_gtest(${target}) received unknown arguments: "
            "${test_UNPARSED_ARGUMENTS}"
        )
    endif()
    if(NOT test_SOURCES)
        message(FATAL_ERROR "mint_add_gtest(${target}) requires SOURCES")
    endif()
    if(NOT test_LABELS)
        message(FATAL_ERROR "mint_add_gtest(${target}) requires LABELS")
    endif()

    add_executable("${target}" ${test_SOURCES})
    mint_configure_target("${target}")

    if(test_GTEST_MAIN)
        set(gtest_target GTest::gtest_main)
    else()
        set(gtest_target GTest::gtest)
    endif()

    target_link_libraries("${target}" PRIVATE ${test_LIBRARIES} "${gtest_target}")
    if(test_INCLUDE_DIRECTORIES)
        target_include_directories("${target}" PRIVATE ${test_INCLUDE_DIRECTORIES})
    endif()

    gtest_add_tests(
        TARGET "${target}"
        SOURCES ${test_SOURCES}
        TEST_PREFIX "${test_TEST_PREFIX}"
        EXTRA_ARGS ${test_EXTRA_ARGS}
        TEST_LIST registered_tests
    )
    set_tests_properties(${registered_tests} PROPERTIES LABELS "${test_LABELS}")
endfunction()
