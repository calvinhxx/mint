find_package(GTest CONFIG REQUIRED)
find_package(Python3 3.10 COMPONENTS Interpreter REQUIRED)
include(MintTesting)

add_subdirectory(support)
add_subdirectory(unit)
add_subdirectory(integration)
add_subdirectory(contract)

if(MINT_BUILD_GUI)
    add_subdirectory(desktop)
endif()

include(cmake/RegisterArchitectureTests.cmake)
include(cmake/RegisterCliTests.cmake)
include(cmake/RegisterWorkflowTests.cmake)

# EN: Keep assertions deterministic regardless of the developer machine's locale. Individual
#     language-selection tests still override this with --lang.
# ZH-CN: 无论开发机器使用何种区域设置，都保持断言结果稳定；各语言选择测试仍通过
#        --lang 单独覆盖该值。
get_property(mint_registered_tests DIRECTORY PROPERTY TESTS)
set_tests_properties(${mint_registered_tests} PROPERTIES ENVIRONMENT "MINT_LANG=zh-CN")
