cmake_minimum_required(VERSION 3.24)

project(v1_broken_calculator LANGUAGES CXX)

add_executable(calculator_tests
    src/calculator.cpp
    tests/calculator_tests.cpp
)
target_compile_features(calculator_tests PRIVATE cxx_std_20)
target_include_directories(calculator_tests PRIVATE include)

enable_testing()
add_test(NAME calculator_tests COMMAND calculator_tests)
