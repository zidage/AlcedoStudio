# Vulkan target ownership (#1036). The targets exist only when ALCEDO_ENABLE_VULKAN is ON.
if(NOT ALCEDO_VULKAN_ENABLED)
    return()
endif()

# Slang kernel build rule (plan section 4.3). The build folder of the kernel modules and their
# inventories; VulkanShaderModuleRegistry reads it when no packaged kernel folder exists.
include("${ALCEDO_SRC_ROOT}/cmake/AlcedoSlangKernels.cmake")
alcedo_find_slang_tools()
set(ALCEDO_VULKAN_KERNEL_BUILD_ROOT "${CMAKE_BINARY_DIR}/shaders/vulkan"
    CACHE INTERNAL "Build folder of the Vulkan kernel modules")

# AlcedoVulkanKernels collects every kernel group. A target that runs kernels from the build
# folder (tests, alcedo-gpu-check, the application) depends on it.
add_custom_target(AlcedoVulkanKernels)
alcedo_add_slang_kernels(AlcedoVulkanKernels
    GROUP test_fill
    SOURCE "${ALCEDO_SRC_ROOT}/vulkan/shader/test_fill.slang"
    KERNELS test_fill test_fill_image
)

set(_alcedo_vulkan_context_sources
    "${ALCEDO_SRC_ROOT}/vulkan/vulkan_api_counters.cpp"
    "${ALCEDO_SRC_ROOT}/vulkan/vulkan_compute_pipeline_cache.cpp"
    "${ALCEDO_SRC_ROOT}/vulkan/vulkan_context.cpp"
    "${ALCEDO_SRC_ROOT}/vulkan/vulkan_device_capabilities.cpp"
    "${ALCEDO_SRC_ROOT}/vulkan/vulkan_device_selection.cpp"
    "${ALCEDO_SRC_ROOT}/vulkan/vulkan_kernel_catalog.cpp"
    "${ALCEDO_SRC_ROOT}/vulkan/vulkan_kernel_inventory.cpp"
    "${ALCEDO_SRC_ROOT}/vulkan/vulkan_memory_allocator.cpp"
    "${ALCEDO_SRC_ROOT}/vulkan/vulkan_pipeline_cache_store.cpp"
    "${ALCEDO_SRC_ROOT}/vulkan/vulkan_requirements.cpp"
    "${ALCEDO_SRC_ROOT}/vulkan/vulkan_result.cpp"
    "${ALCEDO_SRC_ROOT}/vulkan/vulkan_shader_module_registry.cpp"
)

# VulkanContext is a process singleton. When the first-party CUDA targets are DLLs
# (EditPipeline, RawProcessorOp, ...), a static VulkanContext would give each DLL and the
# executable a copy of its own, as the OpenClContext note in opencl/CMakeLists.txt describes.
# The same rule as OpenClContext: a DLL in that configuration, static otherwise.
if(WIN32 AND ALCEDO_CUDA_ENABLED AND ALCEDO_BUILD_CUDA_DLLS)
    def_library(VulkanContext
        TYPE SHARED
        SOURCES ${_alcedo_vulkan_context_sources}
        PUBLIC_DEPS volk::volk_headers GPUOpen::VulkanMemoryAllocator
        PRIVATE_DEPS volk::volk AlcedoLog Sha256Digest
    )
    set_target_properties(VulkanContext PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
    # Same /GL- note as def_cuda_library: the CMake export scan needs regular COFF.
    target_compile_options(VulkanContext PRIVATE
        $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>,$<CONFIG:MinSizeRel>>>:/GL->
    )
    install(TARGETS VulkanContext RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
else()
    def_library(VulkanContext
        SOURCES ${_alcedo_vulkan_context_sources}
        PUBLIC_DEPS volk::volk_headers GPUOpen::VulkanMemoryAllocator
        PRIVATE_DEPS volk::volk AlcedoLog Sha256Digest
    )
endif()
target_compile_definitions(VulkanContext PRIVATE
    ALCEDO_VULKAN_KERNEL_BUILD_ROOT="${ALCEDO_VULKAN_KERNEL_BUILD_ROOT}")
