# Edit domain target ownership (operators, pipeline, accelerator, pipeline history).
# 5. Business Logic & State
# ==============================================================================

set(HISTORY_SRCS
    ${ALCEDO_SRC_ROOT}/edit/history/version_ref.cpp
    ${ALCEDO_SRC_ROOT}/edit/history/edit_commit.cpp
    ${ALCEDO_SRC_ROOT}/edit/history/pipeline_document_checkpoint.cpp
    ${ALCEDO_SRC_ROOT}/edit/history/pipeline_edit_json.cpp
    ${ALCEDO_SRC_ROOT}/edit/history/pipeline_edit_change_validate.cpp
    ${ALCEDO_SRC_ROOT}/edit/history/pipeline_edit_change_json.cpp
    ${ALCEDO_SRC_ROOT}/edit/history/pipeline_edit_batch.cpp
    ${ALCEDO_SRC_ROOT}/edit/history/commit_graph.cpp
    ${ALCEDO_SRC_ROOT}/edit/history/mini_git_working_history.cpp
)

def_library(PipelineHistory
    SOURCES ${HISTORY_SRCS}
    PUBLIC_DEPS Image TimeProvider JSON EditPipeline EditGraph ThreadPool
)

# GPU-free render geometry: coordinate spaces, resolver, sampling plans. No CUDA.
def_library(EditGeometry
    SOURCES
        ${ALCEDO_SRC_ROOT}/edit/geometry/render_geometry_resolver.cpp
        ${ALCEDO_SRC_ROOT}/edit/geometry/texture_sampling_plan.cpp
)

# GPU-free Mask model: analytic sources, common fields, and host Mix coverage.
def_library(EditMaskCore
    SOURCES
        ${ALCEDO_SRC_ROOT}/edit/mask/mask_model.cpp
        ${ALCEDO_SRC_ROOT}/edit/mask/analytic_mask_edit.cpp
        ${ALCEDO_SRC_ROOT}/edit/mask/grade_mask_coverage.cpp
    PUBLIC_DEPS EditGeometry JSON
)

# Detached Brush module: model types, canonical sampling, and spatial/feather
# algorithms only. No rasterization, history, storage, or UI glue. Not shipped.
if(ALCEDO_ENABLE_BRUSH_MASK)
    def_library(EditMaskBrush
        SOURCES
            ${ALCEDO_SRC_ROOT}/edit/mask/mask_asset.cpp
            ${ALCEDO_SRC_ROOT}/edit/mask/brush_stroke.cpp
            ${ALCEDO_SRC_ROOT}/edit/mask/brush_raster_encoding.cpp
            ${ALCEDO_SRC_ROOT}/edit/mask/brush_canonical_sampler.cpp
            ${ALCEDO_SRC_ROOT}/edit/mask/brush_source_geometry.cpp
            ${ALCEDO_SRC_ROOT}/edit/mask/brush_spatial_index.cpp
            ${ALCEDO_SRC_ROOT}/edit/mask/brush_signed_distance.cpp
        PUBLIC_DEPS EditMaskCore EditGeometry JSON
        PRIVATE_DEPS xxHash
    )
endif()

# GPU DAG document/graph/models. No ImageBuffer, Operators, or GPU backends.
set(EDIT_GRAPH_SRCS
    ${ALCEDO_SRC_ROOT}/edit/operators/models/adjustment_catalog.cpp
    ${ALCEDO_SRC_ROOT}/edit/operators/models/cat02_white_balance_model.cpp
    ${ALCEDO_SRC_ROOT}/edit/operators/models/color_wheel_model.cpp
    ${ALCEDO_SRC_ROOT}/edit/operators/models/curve_model.cpp
    ${ALCEDO_SRC_ROOT}/edit/operators/models/hls_model.cpp
    ${ALCEDO_SRC_ROOT}/edit/operators/models/lmt_model.cpp
    ${ALCEDO_SRC_ROOT}/edit/operators/models/sharpen_model.cpp
    ${ALCEDO_SRC_ROOT}/edit/graph/adjustment_ownership.cpp
    ${ALCEDO_SRC_ROOT}/edit/graph/color_grade_node_model.cpp
    ${ALCEDO_SRC_ROOT}/edit/graph/develop_color_transform.cpp
    ${ALCEDO_SRC_ROOT}/edit/graph/develop_node_model.cpp
    ${ALCEDO_SRC_ROOT}/edit/graph/drt_node_model.cpp
    ${ALCEDO_SRC_ROOT}/edit/graph/pipeline_document.cpp
    ${ALCEDO_SRC_ROOT}/edit/graph/pipeline_graph.cpp
    ${ALCEDO_SRC_ROOT}/edit/graph/pipeline_graph_commands.cpp
)

def_library(EditGraph
    SOURCES ${EDIT_GRAPH_SRCS}
    PUBLIC_DEPS JSON EditGeometry EditMaskCore
    PRIVATE_DEPS ${ALCEDO_OPENCV_CORE_DEPS}
)
if(ALCEDO_ENABLE_BRUSH_MASK)
    target_link_libraries(EditGraph PUBLIC EditMaskBrush)
endif()

# CPU RAW unpack / downsample. No CUDA, no ImageBuffer.
def_library(EditInput
    SOURCES
        ${ALCEDO_SRC_ROOT}/edit/input/raw_input_loader.cpp
        ${ALCEDO_SRC_ROOT}/edit/input/prepared_source_cache.cpp
    PUBLIC_DEPS EditGeometry JSON libraw::raw_r opencv_core
)

# GPU-agnostic DAG runtime (parameter arena, texture LRU, KV cache). No CUDA headers.
set(EDIT_RUNTIME_SRCS
    ${ALCEDO_SRC_ROOT}/edit/runtime/adjustment_runtime.cpp
    ${ALCEDO_SRC_ROOT}/edit/runtime/edit_runtime.cpp
    ${ALCEDO_SRC_ROOT}/edit/runtime/grade_lut.cpp
    ${ALCEDO_SRC_ROOT}/edit/runtime/grade_schedule.cpp
    ${ALCEDO_SRC_ROOT}/edit/runtime/drt_post_schedule.cpp
    ${ALCEDO_SRC_ROOT}/edit/runtime/dng_profile_gpu_data.cpp
    ${ALCEDO_SRC_ROOT}/edit/runtime/execution_plan.cpp
    ${ALCEDO_SRC_ROOT}/edit/runtime/graph_compiler.cpp
    ${ALCEDO_SRC_ROOT}/edit/runtime/result_content_key.cpp
    ${ALCEDO_SRC_ROOT}/edit/runtime/runtime_invalidation.cpp
    ${ALCEDO_SRC_ROOT}/edit/runtime/static_execution_plan_cache.cpp
)
if(NOT ALCEDO_METAL_ENABLED)
    list(APPEND EDIT_RUNTIME_SRCS
        ${ALCEDO_SRC_ROOT}/edit/runtime/metal/metal_backend_host.cpp
    )
endif()
# DRT output resolution shared by the CUDA, OpenCL, and Metal DRT passes.
def_library(EditRuntimeDrt
    SOURCES
        ${ALCEDO_SRC_ROOT}/edit/runtime/drt/drt_output_resolver.cpp
        ${ALCEDO_SRC_ROOT}/edit/runtime/drt/aces_odt_runtime.cpp
        ${ALCEDO_SRC_ROOT}/edit/runtime/drt/open_drt_runtime.cpp
    PUBLIC_DEPS EditGraph ${ALCEDO_OPENCV_CORE_DEPS}
)

# Lens calibration shared by the CUDA, OpenCL, and Metal Develop passes: the Lensfun resolver and the
# lens and CUDA geometry kernels.
set(EDIT_RUNTIME_LENS_SRCS
    ${ALCEDO_SRC_ROOT}/edit/runtime/lens/lens_calibration_resolver.cpp
)
if(ALCEDO_CUDA_ENABLED)
    list(APPEND EDIT_RUNTIME_LENS_SRCS
        ${ALCEDO_SRC_ROOT}/edit/runtime/lens/cuda/cuda_geometry_ops.cu
        ${ALCEDO_SRC_ROOT}/edit/runtime/lens/cuda/cuda_lens_calib_ops.cu
    )
endif()
if(ALCEDO_METAL_ENABLED)
    list(APPEND EDIT_RUNTIME_LENS_SRCS
        ${ALCEDO_SRC_ROOT}/edit/runtime/lens/metal/metal_lens_calib.cpp
    )
endif()
if(ALCEDO_OPENCL_ENABLED)
    list(APPEND EDIT_RUNTIME_LENS_SRCS
        ${ALCEDO_SRC_ROOT}/edit/runtime/lens/opencl/opencl_lens_calib_ops.cpp
    )
endif()
def_cuda_library(EditRuntimeLens
    SOURCES ${EDIT_RUNTIME_LENS_SRCS}
    PUBLIC_DEPS EditGraph EditGeometry ${ALCEDO_OPENCV_CORE_DEPS}
    PRIVATE_DEPS puerhlab_lensfun
)
if(ALCEDO_CUDA_ENABLED)
    set_target_properties(EditRuntimeLens PROPERTIES CUDA_RESOLVE_DEVICE_SYMBOLS ON)
    target_link_libraries(EditRuntimeLens PRIVATE CUDA::cudart)
endif()
if(ALCEDO_METAL_ENABLED)
    add_dependencies(EditRuntimeLens LensCalibrationMetalShaders)
    target_link_libraries(EditRuntimeLens PRIVATE MetalGeometryUtils MetalImage MetalContext)
    target_compile_definitions(EditRuntimeLens PRIVATE
        ALCEDO_METAL_LENS_CALIB_METALLIB_PATH="${METAL_LENS_CALIB_METALLIB}"
    )
endif()
if(ALCEDO_OPENCL_ENABLED)
    target_link_libraries(EditRuntimeLens PUBLIC ImageBuffer PRIVATE OpenClContext OpenClProgramLibrary)
endif()
if(DEFINED ALCEDO_LENSFUN_INCLUDE_DIR AND NOT "${ALCEDO_LENSFUN_INCLUDE_DIR}" STREQUAL "")
    target_include_directories(EditRuntimeLens SYSTEM BEFORE PRIVATE "${ALCEDO_LENSFUN_INCLUDE_DIR}")
endif()
if(TARGET puerhlab_lensfun_build)
    add_dependencies(EditRuntimeLens puerhlab_lensfun_build)
endif()
if(WIN32 AND TARGET puerhlab_lensfun)
    add_custom_command(TARGET EditRuntimeLens POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:puerhlab_lensfun>"
            "$<TARGET_FILE_DIR:EditRuntimeLens>/lensfun.dll"
        COMMENT "Alcedo: install submodule lensfun.dll next to EditRuntimeLens"
        VERBATIM)
endif()

# Histogram and waveform analysis of the presented display frame. The CUDA, OpenCL, and Metal
# frame presenters link it.
set(EDIT_SCOPE_SRCS
    ${ALCEDO_SRC_ROOT}/edit/scope/scope_analyzer.cpp
    ${ALCEDO_SRC_ROOT}/edit/scope/final_display_frame_tap.cpp
)
if(ALCEDO_CUDA_ENABLED)
    list(APPEND EDIT_SCOPE_SRCS ${ALCEDO_SRC_ROOT}/edit/scope/cuda_scope_analyzer.cu)
endif()
if(ALCEDO_METAL_ENABLED)
    list(APPEND EDIT_SCOPE_SRCS ${ALCEDO_SRC_ROOT}/edit/scope/metal_scope_analyzer.cpp)
endif()
if(ALCEDO_OPENCL_ENABLED)
    list(APPEND EDIT_SCOPE_SRCS ${ALCEDO_SRC_ROOT}/edit/scope/opencl_scope_analyzer.cpp)
endif()
def_cuda_library(EditScope
    SOURCES ${EDIT_SCOPE_SRCS}
    PUBLIC_DEPS EditGeometry ${ALCEDO_OPENCV_CORE_DEPS}
)
if(ALCEDO_CUDA_ENABLED)
    target_compile_definitions(EditScope PUBLIC HAVE_CUDA)
    target_include_directories(EditScope PRIVATE ${CUDAToolkit_INCLUDE_DIRS})
    target_link_libraries(EditScope PRIVATE CUDA::cudart)
endif()
if(ALCEDO_METAL_ENABLED)
    add_dependencies(EditScope ScopeAnalyzerMetalShaders)
    target_compile_definitions(EditScope PUBLIC HAVE_METAL)
    target_compile_definitions(EditScope PRIVATE
        ALCEDO_METAL_SCOPE_ANALYZER_METALLIB_PATH="${METAL_SCOPE_ANALYZER_METALLIB}"
    )
    target_link_libraries(EditScope PUBLIC MetalContext alcedo::metal_cpp)
endif()
if(ALCEDO_OPENCL_ENABLED)
    target_compile_definitions(EditScope PUBLIC HAVE_OPENCL)
    target_link_libraries(EditScope PUBLIC OpenClContext PRIVATE OpenClProgramLibrary)
endif()

def_library(EditRuntime
    SOURCES ${EDIT_RUNTIME_SRCS}
    PUBLIC_DEPS EditGraph EditGeometry EditInput EditRuntimeDrt
    PRIVATE_DEPS PreviewPerformance
)
if(ALCEDO_ENABLE_BRUSH_MASK)
    target_link_libraries(EditRuntime PUBLIC EditMaskBrush)
endif()

if(ALCEDO_METAL_ENABLED)
    def_library(EditRuntimeMetal
        SOURCES
            ${ALCEDO_SRC_ROOT}/edit/runtime/metal/metal_backend.mm
            ${ALCEDO_SRC_ROOT}/edit/runtime/metal/metal_develop_pass.mm
            ${ALCEDO_SRC_ROOT}/edit/runtime/metal/metal_drt_params.cpp
            ${ALCEDO_SRC_ROOT}/edit/runtime/metal/metal_drt_pass.mm
            ${ALCEDO_SRC_ROOT}/edit/runtime/metal/metal_frame_presenter.mm
            ${ALCEDO_SRC_ROOT}/edit/runtime/metal/metal_local_tone_pass.mm
            ${ALCEDO_SRC_ROOT}/edit/runtime/metal/metal_mask_pass.mm
            ${ALCEDO_SRC_ROOT}/edit/runtime/metal/metal_primary_grade_pass.mm
        PUBLIC_DEPS EditRuntime MetalContext alcedo::metal_cpp
        PRIVATE_DEPS RawProcessorOp EditRuntimeLens EditScope MetalGeometryUtils MetalImage MetalDemosaicNetEntry PreviewPerformance
    )
    target_compile_definitions(EditRuntimeMetal PUBLIC HAVE_METAL)
    target_compile_definitions(EditRuntimeMetal PRIVATE
        ALCEDO_METAL_TO_LINEAR_REF_METALLIB_PATH="${METAL_TO_LINEAR_REF_METALLIB}"
        ALCEDO_METAL_DEBAYER_RCD_METALLIB_PATH="${METAL_DEBAYER_RCD_METALLIB}"
        ALCEDO_METAL_HIGHLIGHT_RECONSTRUCT_METALLIB_PATH="${METAL_HIGHLIGHT_RECONSTRUCT_METALLIB}"
        ALCEDO_METAL_XTRANS_INTERPOLATE_METALLIB_PATH="${METAL_XTRANS_INTERPOLATE_METALLIB}"
        ALCEDO_METAL_CVT_REF_SPACE_METALLIB_PATH="${METAL_CVT_REF_SPACE_METALLIB}"
        ALCEDO_METAL_GEOMETRY_UTILS_METALLIB_PATH="${METAL_GEOMETRY_UTILS_METALLIB}"
        ALCEDO_METAL_GEOMETRY_RESAMPLE_METALLIB_PATH="${METAL_GEOMETRY_RESAMPLE_METALLIB}"
        ALCEDO_METAL_CAMERA_COLOR_METALLIB_PATH="${METAL_CAMERA_COLOR_METALLIB}"
        ALCEDO_METAL_PRIMARY_GRADE_METALLIB_PATH="${METAL_PRIMARY_GRADE_METALLIB}"
        ALCEDO_METAL_LOCAL_TONE_METALLIB_PATH="${METAL_LOCAL_TONE_METALLIB}"
        ALCEDO_METAL_MASK_METALLIB_PATH="${METAL_MASK_METALLIB}"
        ALCEDO_METAL_DRT_METALLIB_PATH="${METAL_DRT_METALLIB}"
        ALCEDO_METAL_DEMOSAICNET_IO_METALLIB_PATH="${METAL_DEMOSAICNET_IO_METALLIB}"
    )
    set_source_files_properties(
        ${ALCEDO_SRC_ROOT}/edit/runtime/metal/metal_backend.mm
        ${ALCEDO_SRC_ROOT}/edit/runtime/metal/metal_develop_pass.mm
        ${ALCEDO_SRC_ROOT}/edit/runtime/metal/metal_drt_pass.mm
        ${ALCEDO_SRC_ROOT}/edit/runtime/metal/metal_frame_presenter.mm
        ${ALCEDO_SRC_ROOT}/edit/runtime/metal/metal_local_tone_pass.mm
        ${ALCEDO_SRC_ROOT}/edit/runtime/metal/metal_mask_pass.mm
        ${ALCEDO_SRC_ROOT}/edit/runtime/metal/metal_primary_grade_pass.mm
        PROPERTIES LANGUAGE OBJCXX
    )
    add_dependencies(EditRuntimeMetal GpuDagMetalShaders RawProcessorOpMetalShaders MetalGeometryUtilsShaders)
    target_link_libraries(EditRuntimeMetal PUBLIC
        "-framework Foundation"
        "-framework Metal"
        "-framework MetalPerformanceShaders"
        "-framework MetalPerformanceShadersGraph"
    )
endif()

if(ALCEDO_CUDA_ENABLED)
    def_cuda_library(EditRuntimeCuda
        SOURCES
            ${ALCEDO_SRC_ROOT}/edit/runtime/cuda/cuda_backend.cpp
            ${ALCEDO_SRC_ROOT}/edit/runtime/cuda/cuda_gpu_timestamp_pool.cpp
            ${ALCEDO_SRC_ROOT}/edit/runtime/cuda/cuda_camera_color_pass.cu
            ${ALCEDO_SRC_ROOT}/edit/runtime/cuda/cuda_develop_pass.cpp
            ${ALCEDO_SRC_ROOT}/edit/runtime/cuda/cuda_render_device.cpp
            ${ALCEDO_SRC_ROOT}/edit/runtime/cuda/cuda_sensor_demosaic.cpp
            ${ALCEDO_SRC_ROOT}/edit/runtime/cuda/cuda_drt_pass.cu
            ${ALCEDO_SRC_ROOT}/edit/runtime/cuda/cuda_drt_runtime_state.cpp
            ${ALCEDO_SRC_ROOT}/edit/runtime/cuda/cuda_neighbor_grade.cu
            ${ALCEDO_SRC_ROOT}/edit/runtime/cuda/cuda_local_tone_pass.cu
            ${ALCEDO_SRC_ROOT}/edit/runtime/cuda/cuda_mask_pass.cu
            ${ALCEDO_SRC_ROOT}/edit/runtime/cuda/cuda_plan_executor.cpp
            ${ALCEDO_SRC_ROOT}/edit/runtime/cuda/cuda_product_renderer.cu
            ${ALCEDO_SRC_ROOT}/edit/runtime/cuda/cuda_primary_grade_pass.cu
            ${ALCEDO_SRC_ROOT}/edit/runtime/cuda/geometry_resample.cu
        PUBLIC_DEPS EditRuntime CUDA::cudart
        PRIVATE_DEPS EditInput RawProcessorOp EditRuntimeLens EditScope CudaDemosaicNetEntry PreviewPerformance
    )
    target_compile_definitions(EditRuntimeCuda PUBLIC HAVE_CUDA)
endif()

if(ALCEDO_OPENCL_ENABLED)
    def_library(EditRuntimeOpenCl
        SOURCES
            ${ALCEDO_SRC_ROOT}/edit/runtime/opencl/opencl_backend.cpp
            ${ALCEDO_SRC_ROOT}/edit/runtime/opencl/opencl_develop_pass.cpp
            ${ALCEDO_SRC_ROOT}/edit/runtime/opencl/opencl_drt_params.cpp
            ${ALCEDO_SRC_ROOT}/edit/runtime/opencl/opencl_drt_pass.cpp
            ${ALCEDO_SRC_ROOT}/edit/runtime/opencl/opencl_frame_presenter.cpp
            ${ALCEDO_SRC_ROOT}/edit/runtime/opencl/opencl_local_tone_pass.cpp
            ${ALCEDO_SRC_ROOT}/edit/runtime/opencl/opencl_mask_pass.cpp
            ${ALCEDO_SRC_ROOT}/edit/runtime/opencl/opencl_primary_grade_pass.cpp
        PUBLIC_DEPS EditRuntime EditInput RawProcessorOp OpenClDemosaicNetEntry OpenClContext OpenClProgramLibrary OpenCL::OpenCL
        PRIVATE_DEPS EditRuntimeLens EditScope OpenClGeometryUtils PreviewPerformance
    )
    target_compile_definitions(EditRuntimeOpenCl PUBLIC HAVE_OPENCL)
endif()

if(ALCEDO_CUDA_ENABLED)
    # The existing scheduler owns PipelineExecutor. Its CUDA branch is a thin adapter into
    # EditRuntimeCuda.
    target_link_libraries(EditPipeline PRIVATE EditRuntimeCuda EditInput EditGraph)
endif()
if(ALCEDO_METAL_ENABLED)
    target_link_libraries(EditPipeline PRIVATE EditRuntimeMetal EditInput EditGraph)
endif()
