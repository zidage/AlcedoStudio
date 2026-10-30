# Run with cmake -DALCEDO_TEST_ARIA2_ARCHIVE=/path/to/the/platform/archive -P this-file.
# Uses the actual release archive; no application compilation or large build tree is needed.
cmake_minimum_required(VERSION 3.21)
if(NOT EXISTS "${ALCEDO_TEST_ARIA2_ARCHIVE}")
    message(FATAL_ERROR "Set ALCEDO_TEST_ARIA2_ARCHIVE to the platform release archive.")
endif()
get_filename_component(_repo "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
get_filename_component(_archive_name "${ALCEDO_TEST_ARIA2_ARCHIVE}" NAME)
string(TIMESTAMP _run "%Y%m%d%H%M%S")
set(_root "${_repo}/build/tmp/fetch-aria2-${_run}")
file(MAKE_DIRECTORY "${_root}")
set(_module "${_repo}/scripts/cmake/fetch_aria2.cmake")

function(run_case name setup expected_rc expected_text)
    set(_dir "${_root}/${name}")
    file(MAKE_DIRECTORY "${_dir}/aria2-download")
    file(COPY "${ALCEDO_TEST_ARIA2_ARCHIVE}" DESTINATION "${_dir}/aria2-download")
    file(WRITE "${_dir}/run.cmake"
        "cmake_minimum_required(VERSION 3.21)\n"
        "set(CMAKE_BINARY_DIR [==[${_dir}]==])\n"
        "${setup}\ninclude([==[${_module}]==])\n"
        "message(STATUS \"RESOLVED=\${ALCEDO_ARIA2C_BINARY}\")\n")
    execute_process(COMMAND "${CMAKE_COMMAND}" -P "${_dir}/run.cmake"
        RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    set(_out "${_out}\n${_err}")
    if(expected_rc STREQUAL "success" AND NOT _rc EQUAL 0)
        message(FATAL_ERROR "${name}: ${_out}")
    elseif(expected_rc STREQUAL "failure" AND _rc EQUAL 0)
        message(FATAL_ERROR "${name}: unexpectedly succeeded: ${_out}")
    endif()
    if(NOT _out MATCHES "${expected_text}")
        message(FATAL_ERROR "${name}: expected ${expected_text}: ${_out}")
    endif()
    message(STATUS "PASS ${name}")
endfunction()

run_case(fresh "" success "aria2c binary:")
file(GLOB_RECURSE _binaries "${_root}/fresh/aria2-download/aria2c"
    "${_root}/fresh/aria2-download/aria2c.exe")
list(GET _binaries 0 _binary)
run_case(override "set(ALCEDO_ARIA2C_BINARY [==[${_binary}]==])"
    success "RESOLVED=${_binary}")
run_case(disabled "set(ALCEDO_BUILD_ARIA2C_FETCH OFF)"
    success "RESOLVED=\n")
run_case(missing_override "set(ALCEDO_ARIA2C_BINARY [==[${_root}/missing]==])"
    failure "ALCEDO_ARIA2C_BINARY does not exist")
run_case(old_cache
    "set(ALCEDO_ARIA2C_BINARY [==[${_root}/old_cache/aria2-download/aria2-osx/aria2c]==] CACHE FILEPATH \"\")\nfile(MAKE_DIRECTORY [==[${_root}/old_cache/aria2-download/aria2-osx]==])\nfile(WRITE \"\${ALCEDO_ARIA2C_BINARY}\" \"old binary\")"
    success "aria2c binary:.*[0-9a-f]+/.*aria2c")
run_case(corrupt_cache
    "file(WRITE [==[${_root}/corrupt_cache/aria2-download/${_archive_name}]==] \"truncated\")"
    failure "archive SHA256 mismatch")
message(STATUS "All aria2 fetch checks passed. Temporary files: ${_root}")
