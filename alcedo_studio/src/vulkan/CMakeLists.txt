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

# Phase VK5: the highlight reconstruction (OpenCL program raw_processor_highlight), the DNG
# WarpRectilinear kernel of the OpenCL program opencl_dag_geometry_camera, and the lens calibration
# (OpenCL program edit_geometry_lens_calib).
alcedo_add_slang_kernels(AlcedoVulkanKernels
    GROUP highlight_reconstruct
    SOURCE "${_alcedo_vulkan_raw_shader_dir}/highlight_reconstruct.slang"
    KERNELS hlr_build_mask hlr_dilate_mask hlr_chrominance_contrib hlr_reconstruct_from_stats
            hlr_build_mask_planar hlr_chrominance_contrib_planar
            hlr_reconstruct_from_stats_planar_pack
    INCLUDE_DIRS "${ALCEDO_SRC_ROOT}"
)
alcedo_add_slang_kernels(AlcedoVulkanKernels
    GROUP dng_warp
    SOURCE "${ALCEDO_SRC_ROOT}/edit/runtime/vulkan/shader/dng_warp.slang"
    KERNELS warp_rectilinear_rgba32f
)
alcedo_add_slang_kernels(AlcedoVulkanKernels
    GROUP lens_calib
    SOURCE "${ALCEDO_SRC_ROOT}/edit/runtime/lens/vulkan/shader/lens_calib.slang"
    KERNELS edit_geometry_lens_vignetting_rgba32f edit_geometry_lens_warp_rgba32f
    INCLUDE_DIRS "${ALCEDO_SRC_ROOT}"
)

# Phase VK7: the geometry resample and the camera color (OpenCL program
# opencl_dag_geometry_camera), and the raster input kernels (opencl_dag_display_to_ap1). They
# include the shared color headers under src/include.
alcedo_add_slang_kernels(AlcedoVulkanKernels
    GROUP geometry_camera
    SOURCE "${ALCEDO_SRC_ROOT}/edit/runtime/vulkan/shader/geometry_camera.slang"
    KERNELS geometry_resample_rgba32f camera_color_acescc
    INCLUDE_DIRS "${ALCEDO_INCLUDE_ROOT}" "${ALCEDO_SRC_ROOT}"
)
alcedo_add_slang_kernels(AlcedoVulkanKernels
    GROUP display_to_ap1
    SOURCE "${ALCEDO_SRC_ROOT}/edit/runtime/vulkan/shader/display_to_ap1.slang"
    KERNELS linearize_raster_rgba display_to_ap1_acescc display_to_ap0_linear
    INCLUDE_DIRS "${ALCEDO_INCLUDE_ROOT}" "${ALCEDO_SRC_ROOT}"
)

# Phase VK8: the point kernels of the Color Grade (OpenCL program opencl_dag_primary_grade).
alcedo_add_slang_kernels(AlcedoVulkanKernels
    GROUP primary_grade
    SOURCE "${ALCEDO_SRC_ROOT}/edit/runtime/vulkan/shader/primary_grade.slang"
    KERNELS primary_grade_pointwise_scene_rgba32f primary_grade_curve_input_histogram
    INCLUDE_DIRS "${ALCEDO_INCLUDE_ROOT}" "${ALCEDO_SRC_ROOT}"
)

# Phase VK9: the neighborhood kernels of the same OpenCL program (primary_grade_neighbor.cl) for
# Clarity, Sharpen, and Halation.
alcedo_add_slang_kernels(AlcedoVulkanKernels
    GROUP primary_grade_neighbor
    SOURCE "${ALCEDO_SRC_ROOT}/edit/runtime/vulkan/shader/primary_grade_neighbor.slang"
    KERNELS primary_grade_neighbor_blur_h_scene_rgba32f primary_grade_neighbor_apply_v_scene_rgba32f
    INCLUDE_DIRS "${ALCEDO_SRC_ROOT}"
)

# Phase VK10: the Local Laplacian stage of the Color Grade (OpenCL program opencl_dag_local_tone,
# local_tone.cl) and three plane kernels on device addresses.
alcedo_add_slang_kernels(AlcedoVulkanKernels
    GROUP local_tone
    SOURCE "${ALCEDO_SRC_ROOT}/edit/runtime/vulkan/shader/local_tone.slang"
    KERNELS local_tone_extract_scene local_tone_extract_reference_scene local_tone_pyr_down
            local_tone_remap local_tone_select local_tone_collapse local_tone_apply_scene
            local_tone_fill_zero local_tone_copy_image_to_plane local_tone_copy_plane_to_image
    INCLUDE_DIRS "${ALCEDO_INCLUDE_ROOT}" "${ALCEDO_SRC_ROOT}"
)

# Phase VK11: MaskEvaluate, MaskUnion, and the coverage displays of a Color Grade Mask (OpenCL
# program opencl_dag_mask, mask.cl), and the AI Mask Feather and Expand (opencl_dag_ai_mask_coverage,
# ai_mask_coverage.cl).
alcedo_add_slang_kernels(AlcedoVulkanKernels
    GROUP mask
    SOURCE "${ALCEDO_SRC_ROOT}/edit/runtime/vulkan/shader/mask.slang"
    KERNELS mask_analytic_r8 mask_raster_r8 mask_raster_rgba32f mask_fill_zero_r8
            mask_union_max_r8 mask_value_display_rgba32f
    INCLUDE_DIRS "${ALCEDO_INCLUDE_ROOT}" "${ALCEDO_SRC_ROOT}"
)
alcedo_add_slang_kernels(AlcedoVulkanKernels
    GROUP ai_mask_coverage
    SOURCE "${ALCEDO_SRC_ROOT}/edit/runtime/vulkan/shader/ai_mask_coverage.slang"
    KERNELS ai_mask_morphology_r8 ai_mask_feather_rows_r32f ai_mask_feather_columns_r8
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
