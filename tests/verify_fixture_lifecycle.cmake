if(NOT DEFINED FIXTURE_DIR OR NOT DEFINED WORK_DIR OR NOT DEFINED CTEST_COMMAND)
    message(FATAL_ERROR "FIXTURE_DIR, WORK_DIR and CTEST_COMMAND are required")
endif()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")
file(COPY "${FIXTURE_DIR}/" DESTINATION "${WORK_DIR}/project")

execute_process(
    COMMAND "${CMAKE_COMMAND}" -S "${WORK_DIR}/project" -B "${WORK_DIR}/project/build"
    RESULT_VARIABLE configure_result
    OUTPUT_VARIABLE configure_output
    ERROR_VARIABLE configure_error
)
if(NOT configure_result EQUAL 0)
    message(FATAL_ERROR "fixture configure failed: ${configure_output}${configure_error}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${WORK_DIR}/project/build" --config Debug --clean-first
    RESULT_VARIABLE build_result
    OUTPUT_VARIABLE build_output
    ERROR_VARIABLE build_error
)
if(NOT build_result EQUAL 0)
    message(FATAL_ERROR "fixture build failed: ${build_output}${build_error}")
endif()

execute_process(
    COMMAND "${CTEST_COMMAND}" --test-dir "${WORK_DIR}/project/build" -C Debug --output-on-failure
    RESULT_VARIABLE failing_test_result
    OUTPUT_VARIABLE failing_test_output
    ERROR_VARIABLE failing_test_error
)
if(failing_test_result EQUAL 0)
    message(FATAL_ERROR "fixture must fail before repair")
endif()
if(NOT failing_test_output MATCHES "add\\(2, 3\\) should be 5")
    message(FATAL_ERROR "fixture failure evidence was unexpected: ${failing_test_output}${failing_test_error}")
endif()

set(implementation "${WORK_DIR}/project/src/calculator.cpp")
file(READ "${implementation}" source)
string(REPLACE "return left - right;" "return left + right;" repaired "${source}")
if(source STREQUAL repaired)
    message(FATAL_ERROR "fixture repair target was not found")
endif()
file(WRITE "${implementation}" "${repaired}")

execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${WORK_DIR}/project/build" --config Debug --clean-first
    RESULT_VARIABLE rebuild_result
    OUTPUT_VARIABLE rebuild_output
    ERROR_VARIABLE rebuild_error
)
if(NOT rebuild_result EQUAL 0)
    message(FATAL_ERROR "fixture rebuild failed: ${rebuild_output}${rebuild_error}")
endif()

execute_process(
    COMMAND "${CTEST_COMMAND}" --test-dir "${WORK_DIR}/project/build" -C Debug --output-on-failure
    RESULT_VARIABLE passing_test_result
    OUTPUT_VARIABLE passing_test_output
    ERROR_VARIABLE passing_test_error
)
if(NOT passing_test_result EQUAL 0)
    message(FATAL_ERROR "fixture must pass after repair: ${passing_test_output}${passing_test_error}")
endif()
