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
# Phase VK3: the check kernels of the shared color math, and the crop and bilinear resize of the
# Develop pass. A kernel includes the shared headers by their paths under src/include and src/.
alcedo_add_slang_kernels(AlcedoVulkanKernels
    GROUP color_math_check
    SOURCE "${ALCEDO_SRC_ROOT}/edit/runtime/vulkan/shader/color_math_check.slang"
    KERNELS color_math_check_transfer color_math_check_color
    INCLUDE_DIRS "${ALCEDO_INCLUDE_ROOT}" "${ALCEDO_SRC_ROOT}"
)
alcedo_add_slang_kernels(AlcedoVulkanKernels
    GROUP geometry_utils
    SOURCE "${ALCEDO_SRC_ROOT}/vulkan/shader/geometry_utils.slang"
    KERNELS geometry_crop_resize_linear
)

# Phase VK4: the RAW operators before the highlight reconstruction, the forms of the OpenCL
# programs raw_processor_core, raw_processor_debayer_rcd, raw_processor_xtrans, and the pack
# kernels of raw_processor_cvt_ref_space.
set(_alcedo_vulkan_raw_shader_dir "${ALCEDO_SRC_ROOT}/decoders/processor/operators/gpu/vulkan_shader")
alcedo_add_slang_kernels(AlcedoVulkanKernels
    GROUP raw_core
    SOURCE "${_alcedo_vulkan_raw_shader_dir}/raw_core.slang"
    KERNELS linearize_rgb to_linear_ref_u16_to_f32 cfa_clamp01_f32
)
alcedo_add_slang_kernels(AlcedoVulkanKernels
    GROUP debayer_rcd
    SOURCE "${_alcedo_vulkan_raw_shader_dir}/debayer_rcd.slang"
    KERNELS rcd_init_and_vh rcd_green_at_rb rcd_pq_dir rcd_rb_at_rb rcd_rb_at_g
    INCLUDE_DIRS "${ALCEDO_SRC_ROOT}"
)
alcedo_add_slang_kernels(AlcedoVulkanKernels
    GROUP xtrans_interpolate
    SOURCE "${_alcedo_vulkan_raw_shader_dir}/xtrans_interpolate.slang"
    KERNELS xtrans_green xtrans_rgba
)
alcedo_add_slang_kernels(AlcedoVulkanKernels
    GROUP raw_pack
    SOURCE "${_alcedo_vulkan_raw_shader_dir}/raw_pack.slang"
    KERNELS pack_planes_crop_inverse_orient copy_rgba_crop_inverse_orient
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
