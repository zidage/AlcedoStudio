# A project with one kernel group and no compiler, for VulkanSlangBuildRuleTest. The test
# configures and builds it with the build rule of the application (AlcedoSlangKernels.cmake).
#
# Inputs: ALCEDO_SRC_ROOT, ALCEDO_SLANGC_EXECUTABLE, ALCEDO_SPIRV_VAL_EXECUTABLE, CHECK_GROUP,
# CHECK_SOURCE, CHECK_KERNELS (comma-separated).
cmake_minimum_required(VERSION 3.24)
project(alcedo_slang_build_rule_check NONE)

include("${ALCEDO_SRC_ROOT}/cmake/AlcedoSlangKernels.cmake")
string(REPLACE "," ";" _kernels "${CHECK_KERNELS}")
alcedo_add_slang_kernels(CheckKernels
    GROUP "${CHECK_GROUP}"
    SOURCE "${CHECK_SOURCE}"
    KERNELS ${_kernels}
    OUTPUT_DIR "${CMAKE_BINARY_DIR}/kernels")
