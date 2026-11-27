# Vulkan target ownership (#1036). The targets exist only when ALCEDO_ENABLE_VULKAN is ON.
if(NOT ALCEDO_VULKAN_ENABLED)
    return()
endif()

set(_alcedo_vulkan_context_sources
    "${ALCEDO_SRC_ROOT}/vulkan/vulkan_context.cpp"
    "${ALCEDO_SRC_ROOT}/vulkan/vulkan_device_capabilities.cpp"
    "${ALCEDO_SRC_ROOT}/vulkan/vulkan_device_selection.cpp"
    "${ALCEDO_SRC_ROOT}/vulkan/vulkan_memory_allocator.cpp"
    "${ALCEDO_SRC_ROOT}/vulkan/vulkan_requirements.cpp"
    "${ALCEDO_SRC_ROOT}/vulkan/vulkan_result.cpp"
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
        PRIVATE_DEPS volk::volk AlcedoLog
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
        PRIVATE_DEPS volk::volk AlcedoLog
    )
endif()
