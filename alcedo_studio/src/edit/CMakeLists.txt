# Edit domain target ownership (operators, pipeline, accelerator, edit history).
# 5. Business Logic & State
# ==============================================================================

set(HISTORY_SRCS
    ${ALCEDO_SRC_ROOT}/edit/history/edit_history.cpp
    ${ALCEDO_SRC_ROOT}/edit/history/version.cpp
    ${ALCEDO_SRC_ROOT}/edit/history/edit_transaction.cpp
    ${ALCEDO_SRC_ROOT}/edit/history/editor_transaction_journal.cpp
    ${ALCEDO_SRC_ROOT}/edit/history/editor_journal_writer.cpp
    ${ALCEDO_SRC_ROOT}/edit/history/editor_journal_recovery.cpp
    ${ALCEDO_SRC_ROOT}/edit/history/version_ref.cpp
    ${ALCEDO_SRC_ROOT}/edit/history/edit_commit.cpp
    ${ALCEDO_SRC_ROOT}/edit/history/commit_graph.cpp
    ${ALCEDO_SRC_ROOT}/edit/history/mini_git_working_history.cpp
)

def_library(EditHistory
    SOURCES ${HISTORY_SRCS}
    PUBLIC_DEPS Image TimeProvider JSON EditPipeline
)

# GPU-free render geometry: coordinate spaces, resolver, sampling plans. No CUDA.
def_library(EditGeometry
    SOURCES
        ${ALCEDO_SRC_ROOT}/edit/geometry/render_geometry_resolver.cpp
        ${ALCEDO_SRC_ROOT}/edit/geometry/texture_sampling_plan.cpp
)

# GPU-free persistent R8 mask repository and host byte-budget cache.
def_library(EditMaskStore
    SOURCES
        ${ALCEDO_SRC_ROOT}/edit/mask/mask_store.cpp
    PUBLIC_DEPS EditGeometry
)

# GPU DAG document/graph/models. No ImageBuffer, Operators, or GPU backends.
set(EDIT_GRAPH_SRCS
    ${ALCEDO_SRC_ROOT}/edit/operators/models/adjustment_catalog.cpp
    ${ALCEDO_SRC_ROOT}/edit/operators/models/cat02_white_balance_model.cpp
    ${ALCEDO_SRC_ROOT}/edit/operators/models/color_wheel_model.cpp
    ${ALCEDO_SRC_ROOT}/edit/operators/models/curve_model.cpp
    ${ALCEDO_SRC_ROOT}/edit/operators/models/hls_model.cpp
    ${ALCEDO_SRC_ROOT}/edit/operators/models/lmt_model.cpp
    ${ALCEDO_SRC_ROOT}/edit/operators/models/sharpen_model.cpp
    ${ALCEDO_SRC_ROOT}/edit/graph/analytic_mask_node_model.cpp
    ${ALCEDO_SRC_ROOT}/edit/graph/color_grade_node_model.cpp
    ${ALCEDO_SRC_ROOT}/edit/graph/develop_color_transform.cpp
    ${ALCEDO_SRC_ROOT}/edit/graph/develop_node_model.cpp
    ${ALCEDO_SRC_ROOT}/edit/graph/drt_node_model.cpp
        ${ALCEDO_SRC_ROOT}/edit/graph/legacy_pipeline_importer.cpp
        ${ALCEDO_SRC_ROOT}/edit/graph/pipeline_document.cpp
        ${ALCEDO_SRC_ROOT}/edit/graph/pipeline_graph.cpp
        ${ALCEDO_SRC_ROOT}/edit/graph/pipeline_graph_commands.cpp
        ${ALCEDO_SRC_ROOT}/edit/graph/raster_mask_node_model.cpp
)

def_library(EditGraph
    SOURCES ${EDIT_GRAPH_SRCS}
    PUBLIC_DEPS JSON EditGeometry EditMaskStore
    PRIVATE_DEPS ${ALCEDO_OPENCV_CORE_DEPS}
)

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
    ${ALCEDO_SRC_ROOT}/edit/runtime/dng_profile_gpu_data.cpp
    ${ALCEDO_SRC_ROOT}/edit/runtime/graph_compiler.cpp
    ${ALCEDO_SRC_ROOT}/edit/runtime/result_content_key.cpp
    ${ALCEDO_SRC_ROOT}/edit/runtime/static_execution_plan_cache.cpp
)
if(NOT ALCEDO_METAL_ENABLED)
    list(APPEND EDIT_RUNTIME_SRCS
        ${ALCEDO_SRC_ROOT}/edit/runtime/metal/metal_backend_host.cpp
    )
endif()
def_library(EditRuntime
    SOURCES ${EDIT_RUNTIME_SRCS}
    PUBLIC_DEPS EditGraph EditGeometry EditInput
)

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
        PRIVATE_DEPS RawProcessorOp Operators MetalGeometryUtils MetalImage MetalDemosaicNetEntry
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
            ${ALCEDO_SRC_ROOT}/edit/runtime/cuda/cuda_camera_color_pass.cu
            ${ALCEDO_SRC_ROOT}/edit/runtime/cuda/cuda_develop_pass.cpp
            ${ALCEDO_SRC_ROOT}/edit/runtime/cuda/cuda_render_device.cpp
            ${ALCEDO_SRC_ROOT}/edit/runtime/cuda/cuda_sensor_demosaic.cpp
            ${ALCEDO_SRC_ROOT}/edit/runtime/cuda/cuda_drt_pass.cu
            ${ALCEDO_SRC_ROOT}/edit/runtime/cuda/cuda_local_tone_pass.cu
            ${ALCEDO_SRC_ROOT}/edit/runtime/cuda/cuda_mask_pass.cu
            ${ALCEDO_SRC_ROOT}/edit/runtime/cuda/cuda_plan_executor.cpp
            ${ALCEDO_SRC_ROOT}/edit/runtime/cuda/cuda_product_renderer.cu
            ${ALCEDO_SRC_ROOT}/edit/runtime/cuda/cuda_primary_grade_pass.cu
            ${ALCEDO_SRC_ROOT}/edit/runtime/cuda/geometry_resample.cu
        PUBLIC_DEPS EditRuntime CUDA::cudart
        PRIVATE_DEPS EditInput RawProcessorOp Operators CudaDemosaicNetEntry
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
        PRIVATE_DEPS Operators
    )
    target_compile_definitions(EditRuntimeOpenCl PUBLIC HAVE_OPENCL)
endif()

if(ALCEDO_CUDA_ENABLED)
    # The existing scheduler owns CPUPipelineExecutor. Its CUDA branch is a thin adapter into
    # EditRuntimeCuda.
    target_link_libraries(EditPipeline PRIVATE EditRuntimeCuda EditInput EditGraph)
endif()
if(ALCEDO_METAL_ENABLED)
    target_link_libraries(EditPipeline PRIVATE EditRuntimeMetal EditInput EditGraph)
endif()
