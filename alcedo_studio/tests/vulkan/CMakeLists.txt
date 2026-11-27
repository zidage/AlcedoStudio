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
