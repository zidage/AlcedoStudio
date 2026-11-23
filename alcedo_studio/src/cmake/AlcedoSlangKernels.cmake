# Slang kernel build rule of the Vulkan backend (#1036, plan section 4.3).
#
# Usage:
#   alcedo_find_slang_tools()
#   alcedo_add_slang_kernels(<target>
#       GROUP <group>
#       SOURCE <file.slang>
#       KERNELS <kernel>...
#       [INCLUDE_DIRS <dir>...]
#       [OUTPUT_DIR <dir>])
#
# alcedo_add_slang_kernels compiles one kernel group to one SPIR-V module with one entry point for
# each kernel (the entry point name is the kernel name), validates the module with spirv-val, and
# writes <OUTPUT_DIR>/<group>.spv and the kernel inventory <OUTPUT_DIR>/<group>.kernels. Each line
# of the inventory is "<kernel><TAB><group>.spv<TAB><SHA-256 of the module>".
# VulkanShaderModuleRegistry reads the inventory. A slangc or spirv-val error stops the build;
# slangc reports the error with the file and the line.
#
# <target> is a custom target that collects the kernel groups. The function adds the custom target
# <target>_<group> and makes <target> depend on it. The default OUTPUT_DIR is
# ALCEDO_VULKAN_KERNEL_BUILD_ROOT.

set(_ALCEDO_SLANG_KERNELS_DIR "${CMAKE_CURRENT_LIST_DIR}")

# Finds slangc and spirv-val of the Vulkan SDK (VX1: slangc 2025.17.2). Stops the configure when
# one is absent.
function(alcedo_find_slang_tools)
    set(_hints "")
    if(DEFINED ENV{VULKAN_SDK})
        file(TO_CMAKE_PATH "$ENV{VULKAN_SDK}" _sdk)
        list(APPEND _hints "${_sdk}/Bin" "${_sdk}/bin")
    endif()
    find_program(ALCEDO_SLANGC_EXECUTABLE NAMES slangc HINTS ${_hints}
        DOC "slangc of the Vulkan SDK (Slang to SPIR-V)")
    find_program(ALCEDO_SPIRV_VAL_EXECUTABLE NAMES spirv-val HINTS ${_hints}
        DOC "spirv-val of the Vulkan SDK")
    if(NOT ALCEDO_SLANGC_EXECUTABLE OR NOT ALCEDO_SPIRV_VAL_EXECUTABLE)
        message(FATAL_ERROR
            "[alcedo] ALCEDO_ENABLE_VULKAN needs slangc and spirv-val. Install the Vulkan SDK "
            "and set VULKAN_SDK, or set ALCEDO_SLANGC_EXECUTABLE and ALCEDO_SPIRV_VAL_EXECUTABLE. "
            "slangc: '${ALCEDO_SLANGC_EXECUTABLE}', spirv-val: '${ALCEDO_SPIRV_VAL_EXECUTABLE}'.")
    endif()
    execute_process(COMMAND "${ALCEDO_SLANGC_EXECUTABLE}" -v
        OUTPUT_VARIABLE _version ERROR_VARIABLE _version_error
        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
    message(STATUS "[alcedo] slangc ${_version}${_version_error}: ${ALCEDO_SLANGC_EXECUTABLE}")
endfunction()

function(alcedo_add_slang_kernels target)
    cmake_parse_arguments(ARG "" "GROUP;SOURCE;OUTPUT_DIR" "KERNELS;INCLUDE_DIRS" ${ARGN})
    if(NOT ARG_GROUP OR NOT ARG_SOURCE OR NOT ARG_KERNELS)
        message(FATAL_ERROR "alcedo_add_slang_kernels(${target}): GROUP, SOURCE, and KERNELS are required.")
    endif()
    if(ARG_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "alcedo_add_slang_kernels(${target}): unknown arguments ${ARG_UNPARSED_ARGUMENTS}.")
    endif()
    if(NOT ALCEDO_SLANGC_EXECUTABLE OR NOT ALCEDO_SPIRV_VAL_EXECUTABLE)
        message(FATAL_ERROR "alcedo_add_slang_kernels(${target}): call alcedo_find_slang_tools() first.")
    endif()
    if(NOT ARG_GROUP MATCHES "^[A-Za-z0-9_]+$")
        message(FATAL_ERROR "alcedo_add_slang_kernels(${target}): group '${ARG_GROUP}' must be a C identifier.")
    endif()
    foreach(_kernel IN LISTS ARG_KERNELS)
        if(NOT _kernel MATCHES "^[A-Za-z_][A-Za-z0-9_]*$")
            message(FATAL_ERROR "alcedo_add_slang_kernels(${target}): kernel '${_kernel}' must be a C identifier.")
        endif()
    endforeach()

    set(_output_dir "${ARG_OUTPUT_DIR}")
    if(NOT _output_dir)
        set(_output_dir "${ALCEDO_VULKAN_KERNEL_BUILD_ROOT}")
    endif()
    if(NOT _output_dir)
        message(FATAL_ERROR "alcedo_add_slang_kernels(${target}): no OUTPUT_DIR and no ALCEDO_VULKAN_KERNEL_BUILD_ROOT.")
    endif()
    get_filename_component(_source "${ARG_SOURCE}" ABSOLUTE)

    # slangc writes into an intermediate folder. The module goes to the output folder only after
    # spirv-val accepts it, so an invalid module never reaches the kernel folder.
    set(_work_dir "${CMAKE_CURRENT_BINARY_DIR}/slang_kernels/${ARG_GROUP}")
    set(_work_module "${_work_dir}/${ARG_GROUP}.spv")
    set(_depfile "${_work_dir}/${ARG_GROUP}.d")
    set(_module "${_output_dir}/${ARG_GROUP}.spv")
    set(_inventory "${_output_dir}/${ARG_GROUP}.kernels")

    set(_entry_args "")
    foreach(_kernel IN LISTS ARG_KERNELS)
        list(APPEND _entry_args -entry "${_kernel}")
    endforeach()
    set(_include_args "")
    foreach(_dir IN LISTS ARG_INCLUDE_DIRS)
        list(APPEND _include_args -I "${_dir}")
    endforeach()
    string(REPLACE ";" "," _kernel_list "${ARG_KERNELS}")

    # Plan section 4.3 (VX1): SPIR-V 1.6, scalar layout, the kernel name as the entry point name.
    add_custom_command(
        # The slangc depfile names the intermediate module, so it is an output of the rule too.
        OUTPUT "${_module}" "${_inventory}" "${_work_module}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${_work_dir}" "${_output_dir}"
        COMMAND "${ALCEDO_SLANGC_EXECUTABLE}" "${_source}"
                -target spirv -profile spirv_1_6 -fvk-use-scalar-layout -fvk-use-entrypoint-name
                -O2 ${_include_args} ${_entry_args}
                -o "${_work_module}" -depfile "${_depfile}"
        COMMAND "${ALCEDO_SPIRV_VAL_EXECUTABLE}" --target-env vulkan1.3 --scalar-block-layout
                "${_work_module}"
        COMMAND "${CMAKE_COMMAND}"
                "-DALCEDO_SLANG_MODULE=${_work_module}"
                "-DALCEDO_SLANG_GROUP=${ARG_GROUP}"
                "-DALCEDO_SLANG_KERNELS=${_kernel_list}"
                "-DALCEDO_SLANG_OUTPUT_DIR=${_output_dir}"
                -P "${_ALCEDO_SLANG_KERNELS_DIR}/write_slang_kernel_inventory.cmake"
        MAIN_DEPENDENCY "${_source}"
        DEPENDS "${_ALCEDO_SLANG_KERNELS_DIR}/write_slang_kernel_inventory.cmake"
        DEPFILE "${_depfile}"
        COMMENT "slangc ${ARG_GROUP}: ${ARG_KERNELS}"
        VERBATIM)

    set(_group_target "${target}_${ARG_GROUP}")
    add_custom_target(${_group_target} DEPENDS "${_module}" "${_inventory}" SOURCES "${_source}")
    if(NOT TARGET ${target})
        add_custom_target(${target})
    endif()
    add_dependencies(${target} ${_group_target})
endfunction()
