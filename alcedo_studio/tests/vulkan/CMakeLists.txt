# Vulkan backend tests (#1036). The targets exist only when ALCEDO_ENABLE_VULKAN is ON.
include("${ALCEDO_TEST_ROOT}/cmake/AlcedoTestRegistration.cmake")

if(NOT ALCEDO_VULKAN_ENABLED)
  return()
endif()

# Requirement check and device selection on recorded property sets; no GPU.
add_executable(VulkanRequirementsTest ${ALCEDO_TEST_ROOT}/vulkan/vulkan_requirements_test.cpp)
target_include_directories(VulkanRequirementsTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(VulkanRequirementsTest PRIVATE GTest::gtest_main VulkanContext)
gtest_discover_tests(VulkanRequirementsTest TEST_PREFIX "VulkanRequirementsTest.")
alcedo_register_test_target(VulkanRequirementsTest core)

# VulkanContext on the local GPUs. volk::volk gives the test an instance of its own to list the
# devices.
add_executable(VulkanContextTest ${ALCEDO_TEST_ROOT}/vulkan/vulkan_context_test.cpp)
target_include_directories(VulkanContextTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(VulkanContextTest PRIVATE GTest::gtest_main VulkanContext volk::volk)
gtest_discover_tests(VulkanContextTest TEST_PREFIX "VulkanContextTest.")
alcedo_register_test_target(VulkanContextTest gpu)

# Kernel inventory, folder rule, and VulkanShaderModuleRegistry on the local GPU with the test_fill
# module of the build folder.
add_executable(VulkanShaderModuleRegistryTest
    ${ALCEDO_TEST_ROOT}/vulkan/vulkan_shader_module_registry_test.cpp)
target_include_directories(VulkanShaderModuleRegistryTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(VulkanShaderModuleRegistryTest PRIVATE GTest::gtest_main VulkanContext)
add_dependencies(VulkanShaderModuleRegistryTest AlcedoVulkanKernels)
gtest_discover_tests(VulkanShaderModuleRegistryTest TEST_PREFIX "VulkanShaderModuleRegistryTest.")
alcedo_register_test_target(VulkanShaderModuleRegistryTest gpu)

# VulkanComputePipelineCache on the local GPU: pipelines that run the test_fill kernels, and the
# stored VkPipelineCache data.
add_executable(VulkanComputePipelineCacheTest
    ${ALCEDO_TEST_ROOT}/vulkan/vulkan_compute_pipeline_cache_test.cpp)
target_include_directories(VulkanComputePipelineCacheTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(VulkanComputePipelineCacheTest PRIVATE GTest::gtest_main VulkanContext)
add_dependencies(VulkanComputePipelineCacheTest AlcedoVulkanKernels)
gtest_discover_tests(VulkanComputePipelineCacheTest TEST_PREFIX "VulkanComputePipelineCacheTest.")
alcedo_register_test_target(VulkanComputePipelineCacheTest gpu)

# The Slang build rule (AlcedoSlangKernels.cmake) on a project with no compiler: a Slang error
# stops the build with the file and the line; a valid group writes its module and inventory; a
# header change builds the module again. Needs no GPU.
foreach(_case IN ITEMS
        SlangErrorStopsTheBuildWithTheFileAndTheLine
        ValidGroupWritesTheModuleAndOneInventoryLineForEachKernel
        HeaderChangeBuildsTheModuleAgain)
    add_test(NAME VulkanSlangBuildRuleTest.${_case}
        COMMAND "${CMAKE_COMMAND}"
            "-DCASE=${_case}"
            "-DALCEDO_SRC_ROOT=${ALCEDO_SRC_DIR}"
            "-DALCEDO_SLANGC_EXECUTABLE=${ALCEDO_SLANGC_EXECUTABLE}"
            "-DALCEDO_SPIRV_VAL_EXECUTABLE=${ALCEDO_SPIRV_VAL_EXECUTABLE}"
            "-DCHECK_GENERATOR=${CMAKE_GENERATOR}"
            "-DCHECK_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
            "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/slang_build_rule"
            -P "${ALCEDO_TEST_ROOT}/vulkan/slang_build_rule/check_slang_build_rule.cmake")
endforeach()
