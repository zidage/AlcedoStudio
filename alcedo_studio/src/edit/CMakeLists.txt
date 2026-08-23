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
def_library(EditRuntime
    SOURCES
        ${ALCEDO_SRC_ROOT}/edit/runtime/edit_runtime.cpp
        ${ALCEDO_SRC_ROOT}/edit/runtime/graph_compiler.cpp
        ${ALCEDO_SRC_ROOT}/edit/runtime/result_content_key.cpp
        ${ALCEDO_SRC_ROOT}/edit/runtime/static_execution_plan_cache.cpp
    PUBLIC_DEPS EditGraph EditGeometry EditInput
)

if(ALCEDO_CUDA_ENABLED)
    def_cuda_library(EditRuntimeCuda
        SOURCES
            ${ALCEDO_SRC_ROOT}/edit/runtime/cuda/cuda_backend.cpp
            ${ALCEDO_SRC_ROOT}/edit/runtime/cuda/cuda_adjustment_runtime.cpp
            ${ALCEDO_SRC_ROOT}/edit/runtime/cuda/cuda_camera_color_pass.cu
            ${ALCEDO_SRC_ROOT}/edit/runtime/cuda/cuda_develop_pass.cpp
            ${ALCEDO_SRC_ROOT}/edit/runtime/cuda/cuda_drt_pass.cu
            ${ALCEDO_SRC_ROOT}/edit/runtime/cuda/cuda_mask_pass.cu
            ${ALCEDO_SRC_ROOT}/edit/runtime/cuda/cuda_plan_executor.cpp
            ${ALCEDO_SRC_ROOT}/edit/runtime/cuda/cuda_product_renderer.cu
            ${ALCEDO_SRC_ROOT}/edit/runtime/cuda/cuda_primary_grade_pass.cu
            ${ALCEDO_SRC_ROOT}/edit/runtime/cuda/geometry_resample.cu
        PUBLIC_DEPS EditRuntime CUDA::cudart
        PRIVATE_DEPS EditInput RawProcessorOp Operators
    )
    target_compile_definitions(EditRuntimeCuda PUBLIC HAVE_CUDA)
endif()

if(ALCEDO_CUDA_ENABLED)
    # The existing scheduler owns CPUPipelineExecutor. Its CUDA branch is now a thin adapter into
    # EditRuntimeCuda; OpenCL and Metal continue using the established stage adapters.
    target_link_libraries(EditPipeline PRIVATE EditRuntimeCuda EditInput EditGraph)
endif()
