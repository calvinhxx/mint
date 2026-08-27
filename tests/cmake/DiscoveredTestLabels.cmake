if(mint_v1_2_tests_TESTS)
    set_tests_properties(${mint_v1_2_tests_TESTS} PROPERTIES LABELS "v1.2;contract")
endif()

if(mint_v1_3_tests_TESTS)
    set_tests_properties(${mint_v1_3_tests_TESTS} PROPERTIES LABELS "v1.3;contract")
endif()

if(mint_v1_4_tests_TESTS)
    set_tests_properties(
        ${mint_v1_4_tests_TESTS}
        PROPERTIES LABELS "v1.4;contract;acceptance"
    )
endif()
