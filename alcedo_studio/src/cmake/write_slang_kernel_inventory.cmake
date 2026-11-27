# Script step of alcedo_add_slang_kernels (AlcedoSlangKernels.cmake): copies the module that
# spirv-val accepted into the kernel folder and writes the kernel inventory of its group.
#
# Inputs: ALCEDO_SLANG_MODULE (validated module), ALCEDO_SLANG_GROUP, ALCEDO_SLANG_KERNELS
# (comma-separated kernel names), ALCEDO_SLANG_OUTPUT_DIR.
foreach(_input IN ITEMS ALCEDO_SLANG_MODULE ALCEDO_SLANG_GROUP ALCEDO_SLANG_KERNELS
                        ALCEDO_SLANG_OUTPUT_DIR)
    if(NOT ${_input})
        message(FATAL_ERROR "write_slang_kernel_inventory.cmake: ${_input} is not set.")
    endif()
endforeach()

set(_module_name "${ALCEDO_SLANG_GROUP}.spv")
set(_module "${ALCEDO_SLANG_OUTPUT_DIR}/${_module_name}")
file(MAKE_DIRECTORY "${ALCEDO_SLANG_OUTPUT_DIR}")
file(COPY_FILE "${ALCEDO_SLANG_MODULE}" "${_module}")
file(SHA256 "${_module}" _checksum)

string(REPLACE "," ";" _kernels "${ALCEDO_SLANG_KERNELS}")
set(_lines "")
foreach(_kernel IN LISTS _kernels)
    string(APPEND _lines "${_kernel}\t${_module_name}\t${_checksum}\n")
endforeach()
file(WRITE "${ALCEDO_SLANG_OUTPUT_DIR}/${ALCEDO_SLANG_GROUP}.kernels" "${_lines}")
