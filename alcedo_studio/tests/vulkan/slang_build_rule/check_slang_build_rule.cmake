# VulkanSlangBuildRuleTest: configures and builds the project of this folder with the Slang build
# rule of the application, and checks the result.
#
# cmake -DCASE=<case> -DALCEDO_SRC_ROOT=... -DALCEDO_SLANGC_EXECUTABLE=... \
#       -DALCEDO_SPIRV_VAL_EXECUTABLE=... -DCHECK_GENERATOR=... -DCHECK_MAKE_PROGRAM=... \
#       -DWORK_DIR=... -P check_slang_build_rule.cmake
#
# CASE SlangErrorStopsTheBuildWithTheFileAndTheLine: the build of kernel_with_undefined_name.slang
#   fails, the output names the file and line 7, and no module or inventory is written.
# CASE ValidGroupWritesTheModuleAndOneInventoryLineForEachKernel: the build of
#   fill_with_header.slang writes the module and one inventory line for each kernel with the
#   SHA-256 of the module.
# CASE HeaderChangeBuildsTheModuleAgain: after a change of the included header, the build writes
#   a module with another SHA-256 and an inventory with the new value.

foreach(_input IN ITEMS CASE ALCEDO_SRC_ROOT ALCEDO_SLANGC_EXECUTABLE ALCEDO_SPIRV_VAL_EXECUTABLE
                        CHECK_GENERATOR WORK_DIR)
    if(NOT ${_input})
        message(FATAL_ERROR "check_slang_build_rule.cmake: ${_input} is not set.")
    endif()
endforeach()

set(_project_dir "${CMAKE_CURRENT_LIST_DIR}")
set(_case_dir "${WORK_DIR}/${CASE}")
set(_source_dir "${_case_dir}/src")
set(_build_dir "${_case_dir}/build")
file(REMOVE_RECURSE "${_case_dir}")
file(MAKE_DIRECTORY "${_source_dir}")
# The test changes the header in a copy, never in the source tree.
file(COPY "${_project_dir}/fill_with_header.slang" "${_project_dir}/fill_value.slangh"
          "${_project_dir}/kernel_with_undefined_name.slang"
     DESTINATION "${_source_dir}")

if(CASE STREQUAL "SlangErrorStopsTheBuildWithTheFileAndTheLine")
    set(_group "kernel_with_undefined_name")
    set(_kernels "kernel_with_undefined_name")
else()
    set(_group "fill_with_header")
    set(_kernels "fill_header_value,fill_header_value_twice")
endif()

function(_check_configure)
    set(_make_program_arg "")
    if(CHECK_MAKE_PROGRAM)
        set(_make_program_arg "-DCMAKE_MAKE_PROGRAM=${CHECK_MAKE_PROGRAM}")
    endif()
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -S "${_project_dir}" -B "${_build_dir}" -G "${CHECK_GENERATOR}"
                ${_make_program_arg}
                "-DALCEDO_SRC_ROOT=${ALCEDO_SRC_ROOT}"
                "-DALCEDO_SLANGC_EXECUTABLE=${ALCEDO_SLANGC_EXECUTABLE}"
                "-DALCEDO_SPIRV_VAL_EXECUTABLE=${ALCEDO_SPIRV_VAL_EXECUTABLE}"
                "-DCHECK_GROUP=${_group}"
                "-DCHECK_SOURCE=${_source_dir}/${_group}.slang"
                "-DCHECK_KERNELS=${_kernels}"
        RESULT_VARIABLE _result OUTPUT_VARIABLE _output ERROR_VARIABLE _output)
    if(NOT _result EQUAL 0)
        message(FATAL_ERROR "The configure of the check project failed:\n${_output}")
    endif()
endfunction()

# Builds the kernel target. Sets _build_result and _build_output in the caller.
macro(_check_build)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" --build "${_build_dir}" --target CheckKernels
        RESULT_VARIABLE _build_result OUTPUT_VARIABLE _build_output ERROR_VARIABLE _build_output)
endmacro()

# Checks the inventory: one line for each kernel, with the module name and its SHA-256.
function(_check_inventory expected_kernels)
    set(_module "${_build_dir}/kernels/${_group}.spv")
    set(_inventory "${_build_dir}/kernels/${_group}.kernels")
    if(NOT EXISTS "${_module}" OR NOT EXISTS "${_inventory}")
        message(FATAL_ERROR "The build wrote no module or no inventory in ${_build_dir}/kernels.")
    endif()
    file(SHA256 "${_module}" _checksum)
    file(READ "${_inventory}" _text)
    set(_expected "")
    string(REPLACE "," ";" _kernel_list "${expected_kernels}")
    foreach(_kernel IN LISTS _kernel_list)
        string(APPEND _expected "${_kernel}\t${_group}.spv\t${_checksum}\n")
    endforeach()
    if(NOT _text STREQUAL _expected)
        message(FATAL_ERROR "The inventory is\n${_text}\nexpected\n${_expected}")
    endif()
    set(_module_checksum "${_checksum}" PARENT_SCOPE)
endfunction()

_check_configure()
_check_build()

if(CASE STREQUAL "SlangErrorStopsTheBuildWithTheFileAndTheLine")
    if(_build_result EQUAL 0)
        message(FATAL_ERROR "The build of a kernel with an error did not fail:\n${_build_output}")
    endif()
    if(NOT _build_output MATCHES "kernel_with_undefined_name\\.slang\\(7\\): error")
        message(FATAL_ERROR "The build output does not name the file and line 7:\n${_build_output}")
    endif()
    if(EXISTS "${_build_dir}/kernels/${_group}.spv" OR EXISTS "${_build_dir}/kernels/${_group}.kernels")
        message(FATAL_ERROR "The failed build wrote a module or an inventory.")
    endif()
    message(STATUS "PASS: the build stopped with kernel_with_undefined_name.slang(7).")
    return()
endif()

if(NOT _build_result EQUAL 0)
    message(FATAL_ERROR "The build of a valid kernel group failed:\n${_build_output}")
endif()
_check_inventory("${_kernels}")

if(CASE STREQUAL "ValidGroupWritesTheModuleAndOneInventoryLineForEachKernel")
    message(STATUS "PASS: module and inventory of ${_kernels}.")
    return()
endif()

if(CASE STREQUAL "HeaderChangeBuildsTheModuleAgain")
    set(_first_checksum "${_module_checksum}")
    file(WRITE "${_source_dir}/fill_value.slangh" "static const uint kFillValue = 11;\n")
    _check_build()
    if(NOT _build_result EQUAL 0)
        message(FATAL_ERROR "The build after the header change failed:\n${_build_output}")
    endif()
    _check_inventory("${_kernels}")
    if(_module_checksum STREQUAL _first_checksum)
        message(FATAL_ERROR "The header change did not build the module again:\n${_build_output}")
    endif()
    message(STATUS "PASS: the header change gave a new module ${_module_checksum}.")
    return()
endif()

message(FATAL_ERROR "Unknown CASE ${CASE}.")
