if(NOT DEFINED AIAGENT_EXECUTABLE OR NOT DEFINED FIXTURE_DIR OR NOT DEFINED STATE_DIR)
    message(FATAL_ERROR "AIAGENT_EXECUTABLE, FIXTURE_DIR and STATE_DIR are required")
endif()

file(REMOVE_RECURSE "${STATE_DIR}")
file(REMOVE_RECURSE "${STATE_DIR}-errors")

execute_process(
    COMMAND "${AIAGENT_EXECUTABLE}" init --root "${FIXTURE_DIR}" --state-dir "${STATE_DIR}" --json
    RESULT_VARIABLE init_result
    OUTPUT_VARIABLE init_output
    ERROR_VARIABLE init_error
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT init_result EQUAL 0)
    message(FATAL_ERROR "v1.3 init failed: ${init_output}${init_error}")
endif()
string(JSON init_status GET "${init_output}" status)
string(JSON project_kind GET "${init_output}" project_kind)
string(JSON managed_state GET "${init_output}" state_directory)
if(NOT init_status STREQUAL "initialized" OR NOT project_kind STREQUAL "cmake")
    message(FATAL_ERROR "v1.3 init returned an unexpected contract: ${init_output}")
endif()
string(FIND "${managed_state}" "${FIXTURE_DIR}" state_inside_workspace)
if(NOT state_inside_workspace EQUAL -1)
    message(FATAL_ERROR "managed state must remain outside the workspace: ${managed_state}")
endif()

execute_process(
    COMMAND "${AIAGENT_EXECUTABLE}" status --root "${FIXTURE_DIR}" --state-dir "${STATE_DIR}" --json
    RESULT_VARIABLE empty_status_result
    OUTPUT_VARIABLE empty_status_output
    ERROR_VARIABLE empty_status_error
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT empty_status_result EQUAL 0)
    message(FATAL_ERROR "empty v1.3 status failed: ${empty_status_output}${empty_status_error}")
endif()
string(JSON empty_task_count LENGTH "${empty_status_output}" tasks)
if(NOT empty_task_count EQUAL 0)
    message(FATAL_ERROR "newly initialized project must not have tasks: ${empty_status_output}")
endif()

execute_process(
    COMMAND "${AIAGENT_EXECUTABLE}" run --root "${FIXTURE_DIR}" --state-dir "${STATE_DIR}" --demo --json "inspect this fixture"
    RESULT_VARIABLE run_result
    OUTPUT_VARIABLE run_output
    ERROR_VARIABLE run_error
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT run_result EQUAL 0)
    message(FATAL_ERROR "managed demo run failed: ${run_output}${run_error}")
endif()
string(JSON run_status GET "${run_output}" status)
string(JSON run_completed GET "${run_output}" completed)
string(JSON task_id GET "${run_output}" task_id)
string(JSON task_directory GET "${run_output}" task_directory)
if(NOT run_status STREQUAL "completed" OR NOT run_completed OR task_id STREQUAL "")
    message(FATAL_ERROR "managed demo run returned an unexpected contract: ${run_output}")
endif()
string(FIND "${task_directory}" "${FIXTURE_DIR}" task_inside_workspace)
if(NOT task_inside_workspace EQUAL -1)
    message(FATAL_ERROR "task state must remain outside the workspace: ${task_directory}")
endif()

execute_process(
    COMMAND "${AIAGENT_EXECUTABLE}" status --root "${FIXTURE_DIR}" --state-dir "${STATE_DIR}" --json
    RESULT_VARIABLE populated_status_result
    OUTPUT_VARIABLE populated_status_output
    ERROR_VARIABLE populated_status_error
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT populated_status_result EQUAL 0)
    message(FATAL_ERROR "populated v1.3 status failed: ${populated_status_output}${populated_status_error}")
endif()
string(JSON task_count LENGTH "${populated_status_output}" tasks)
string(JSON listed_task_id GET "${populated_status_output}" tasks 0 id)
string(JSON listed_status GET "${populated_status_output}" tasks 0 status)
string(JSON listed_mode GET "${populated_status_output}" tasks 0 mode)
string(JSON listed_resumable GET "${populated_status_output}" tasks 0 resumable)
if(NOT task_count EQUAL 1 OR NOT listed_task_id STREQUAL task_id OR
   NOT listed_status STREQUAL "completed" OR NOT listed_mode STREQUAL "demo" OR listed_resumable)
    message(FATAL_ERROR "status did not report the completed task: ${populated_status_output}")
endif()

execute_process(
    COMMAND "${AIAGENT_EXECUTABLE}" resume --root "${FIXTURE_DIR}" --state-dir "${STATE_DIR}" --task "${task_id}" --json
    RESULT_VARIABLE resume_result
    OUTPUT_VARIABLE resume_output
    ERROR_VARIABLE resume_error
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(resume_result EQUAL 0)
    message(FATAL_ERROR "completed task must not be resumable: ${resume_output}${resume_error}")
endif()
string(JSON resume_status GET "${resume_output}" status)
string(JSON resume_message GET "${resume_output}" error)
if(NOT resume_status STREQUAL "error" OR NOT resume_message MATCHES "不可恢复")
    message(FATAL_ERROR "completed-task rejection was not explicit: ${resume_output}${resume_error}")
endif()

execute_process(
    COMMAND "${AIAGENT_EXECUTABLE}" init --root "${FIXTURE_DIR}" --state-dir "${STATE_DIR}" --json
    RESULT_VARIABLE duplicate_init_result
    OUTPUT_VARIABLE duplicate_init_output
    ERROR_VARIABLE duplicate_init_error
)
if(duplicate_init_result EQUAL 0)
    message(FATAL_ERROR "duplicate init must require --force: ${duplicate_init_output}${duplicate_init_error}")
endif()

execute_process(
    COMMAND "${AIAGENT_EXECUTABLE}" init --root "${FIXTURE_DIR}" --state-dir "${STATE_DIR}" --force --json
    RESULT_VARIABLE force_init_result
    OUTPUT_VARIABLE force_init_output
    ERROR_VARIABLE force_init_error
)
if(NOT force_init_result EQUAL 0)
    message(FATAL_ERROR "forced init failed: ${force_init_output}${force_init_error}")
endif()

execute_process(
    COMMAND "${AIAGENT_EXECUTABLE}" status --root "${FIXTURE_DIR}" --state-dir "${STATE_DIR}" --json
    RESULT_VARIABLE post_force_status_result
    OUTPUT_VARIABLE post_force_status_output
    ERROR_VARIABLE post_force_status_error
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT post_force_status_result EQUAL 0)
    message(FATAL_ERROR "post-force status failed: ${post_force_status_output}${post_force_status_error}")
endif()
string(JSON post_force_task_count LENGTH "${post_force_status_output}" tasks)
if(NOT post_force_task_count EQUAL 1)
    message(FATAL_ERROR "forced init must preserve existing tasks: ${post_force_status_output}")
endif()

execute_process(
    COMMAND "${AIAGENT_EXECUTABLE}" status --root "${FIXTURE_DIR}" --state-dir "${STATE_DIR}" --demo --json
    RESULT_VARIABLE invalid_status_result
    OUTPUT_VARIABLE invalid_status_output
    ERROR_VARIABLE invalid_status_error
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(invalid_status_result EQUAL 0)
    message(FATAL_ERROR "status must reject irrelevant model options")
endif()
string(JSON invalid_status GET "${invalid_status_output}" status)
if(NOT invalid_status STREQUAL "error")
    message(FATAL_ERROR "invalid status must use the JSON error contract: ${invalid_status_output}${invalid_status_error}")
endif()

execute_process(
    COMMAND "${AIAGENT_EXECUTABLE}" init --root "${FIXTURE_DIR}" --state-dir "${FIXTURE_DIR}/.aiagent-state" --json
    RESULT_VARIABLE unsafe_state_result
    OUTPUT_VARIABLE unsafe_state_output
    ERROR_VARIABLE unsafe_state_error
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(unsafe_state_result EQUAL 0)
    message(FATAL_ERROR "state directory inside the workspace must be rejected")
endif()
string(JSON unsafe_state_status GET "${unsafe_state_output}" status)
if(NOT unsafe_state_status STREQUAL "error")
    message(FATAL_ERROR "unsafe state rejection must use the JSON error contract: ${unsafe_state_output}${unsafe_state_error}")
endif()

execute_process(
    COMMAND "${AIAGENT_EXECUTABLE}" init --root "${FIXTURE_DIR}" --state-dir "${STATE_DIR}-errors" --json
    RESULT_VARIABLE error_init_result
    OUTPUT_VARIABLE error_init_output
    ERROR_VARIABLE error_init_error
)
if(NOT error_init_result EQUAL 0)
    message(FATAL_ERROR "error-contract fixture init failed: ${error_init_output}${error_init_error}")
endif()
execute_process(
    COMMAND "${AIAGENT_EXECUTABLE}" run --root "${FIXTURE_DIR}" --state-dir "${STATE_DIR}-errors" --config "${STATE_DIR}-errors/missing-config.json" --json "report a setup failure"
    RESULT_VARIABLE failed_run_result
    OUTPUT_VARIABLE failed_run_output
    ERROR_VARIABLE failed_run_error
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(failed_run_result EQUAL 0)
    message(FATAL_ERROR "missing model config must fail the managed run")
endif()
string(JSON failed_run_status GET "${failed_run_output}" status)
string(JSON failed_run_task_id GET "${failed_run_output}" task_id)
if(NOT failed_run_status STREQUAL "error" OR failed_run_task_id STREQUAL "")
    message(FATAL_ERROR "managed run errors must retain their task identity: ${failed_run_output}${failed_run_error}")
endif()
execute_process(
    COMMAND "${AIAGENT_EXECUTABLE}" status --root "${FIXTURE_DIR}" --state-dir "${STATE_DIR}-errors" --task "${failed_run_task_id}" --json
    RESULT_VARIABLE failed_task_status_result
    OUTPUT_VARIABLE failed_task_status_output
    ERROR_VARIABLE failed_task_status_error
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT failed_task_status_result EQUAL 0)
    message(FATAL_ERROR "failed task status lookup failed: ${failed_task_status_output}${failed_task_status_error}")
endif()
string(JSON failed_task_state GET "${failed_task_status_output}" tasks 0 status)
if(NOT failed_task_state STREQUAL "created")
    message(FATAL_ERROR "pre-checkpoint failure must remain visible as created: ${failed_task_status_output}")
endif()

execute_process(
    COMMAND "${AIAGENT_EXECUTABLE}" --demo --root "${FIXTURE_DIR}" --json "legacy compatibility"
    RESULT_VARIABLE legacy_result
    OUTPUT_VARIABLE legacy_output
    ERROR_VARIABLE legacy_error
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT legacy_result EQUAL 0)
    message(FATAL_ERROR "legacy demo compatibility failed: ${legacy_output}${legacy_error}")
endif()
string(JSON legacy_status GET "${legacy_output}" status)
string(FIND "${legacy_output}" "\"task_id\"" legacy_task_field)
if(NOT legacy_status STREQUAL "completed" OR NOT legacy_task_field EQUAL -1)
    message(FATAL_ERROR "legacy invocation contract unexpectedly changed: ${legacy_output}")
endif()
