if(NOT DEFINED MINT_EXECUTABLE OR NOT DEFINED SOURCE_DIR)
    message(FATAL_ERROR "MINT_EXECUTABLE and SOURCE_DIR are required")
endif()

execute_process(
    COMMAND "${MINT_EXECUTABLE}" run --json --interaction-jsonl --log-level info
            --log-file-level invalid --demo probe
    RESULT_VARIABLE interaction_result
    OUTPUT_VARIABLE interaction_stdout
    ERROR_VARIABLE interaction_stderr
)
if(interaction_result EQUAL 0 OR NOT interaction_stderr STREQUAL "")
    message(FATAL_ERROR "early interaction failure polluted stderr or unexpectedly succeeded")
endif()
string(JSON interaction_status ERROR_VARIABLE json_error GET "${interaction_stdout}" status)
if(json_error OR NOT interaction_status STREQUAL "error")
    message(FATAL_ERROR "early interaction failure did not return a JSON error")
endif()

execute_process(
    COMMAND "${MINT_EXECUTABLE}" --demo --root "${SOURCE_DIR}" --log-dir "${SOURCE_DIR}"
            --log-file-level off --json probe
    RESULT_VARIABLE disabled_result
    OUTPUT_VARIABLE disabled_stdout
    ERROR_VARIABLE disabled_stderr
)
if(NOT disabled_result EQUAL 0 OR NOT disabled_stderr STREQUAL "")
    message(FATAL_ERROR "disabled file logging still evaluated or emitted diagnostics")
endif()
string(JSON disabled_status ERROR_VARIABLE json_error GET "${disabled_stdout}" status)
if(json_error OR NOT disabled_status STREQUAL "completed")
    message(FATAL_ERROR "disabled file logging did not preserve CLI execution")
endif()

execute_process(
    COMMAND "${MINT_EXECUTABLE}" --demo --root "${SOURCE_DIR}" --log-dir "${SOURCE_DIR}/.."
            --json probe
    RESULT_VARIABLE ancestor_result
    OUTPUT_VARIABLE ancestor_stdout
    ERROR_VARIABLE ancestor_stderr
)
if(ancestor_result EQUAL 0)
    message(FATAL_ERROR "an ancestor of the workspace was accepted as a log directory")
endif()

if(WIN32)
    set(temp_root "$ENV{TEMP}")
else()
    set(temp_root "$ENV{TMPDIR}")
endif()
if(temp_root STREQUAL "")
    set(temp_root "/tmp")
endif()
string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef probe_id)
set(explicit_log_dir "${temp_root}/mint-日志-${probe_id}")
file(REMOVE_RECURSE "${explicit_log_dir}")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "HOME=" "USERPROFILE=" "LOCALAPPDATA="
            "${MINT_EXECUTABLE}" --demo --root "${SOURCE_DIR}" --log-dir "${explicit_log_dir}"
            --json probe
    RESULT_VARIABLE explicit_result
    OUTPUT_VARIABLE explicit_stdout
    ERROR_VARIABLE explicit_stderr
)
string(JSON explicit_status ERROR_VARIABLE json_error GET "${explicit_stdout}" status)
file(GLOB explicit_logs "${explicit_log_dir}/mint-*.jsonl")
file(REMOVE_RECURSE "${explicit_log_dir}")
if(NOT explicit_result EQUAL 0 OR json_error OR NOT explicit_status STREQUAL "completed" OR
   explicit_logs STREQUAL "")
    message(FATAL_ERROR
        "explicit Unicode log directory failed without default state environment\n"
        "result=${explicit_result}\n"
        "log_dir=${explicit_log_dir}\n"
        "logs=${explicit_logs}\n"
        "stdout=${explicit_stdout}\n"
        "stderr=${explicit_stderr}"
    )
endif()
