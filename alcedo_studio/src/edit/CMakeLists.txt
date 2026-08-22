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
    ${ALCEDO_SRC_ROOT}/edit/graph/develop_node_model.cpp
    ${ALCEDO_SRC_ROOT}/edit/graph/drt_node_model.cpp
    ${ALCEDO_SRC_ROOT}/edit/graph/legacy_pipeline_importer.cpp
    ${ALCEDO_SRC_ROOT}/edit/graph/pipeline_document.cpp
    ${ALCEDO_SRC_ROOT}/edit/graph/pipeline_graph.cpp
    ${ALCEDO_SRC_ROOT}/edit/graph/raster_mask_node_model.cpp
)

def_library(EditGraph
    SOURCES ${EDIT_GRAPH_SRCS}
    PUBLIC_DEPS JSON EditGeometry
)

# CPU RAW unpack / downsample. No CUDA, no ImageBuffer.
def_library(EditInput
    SOURCES ${ALCEDO_SRC_ROOT}/edit/input/raw_input_loader.cpp
    PUBLIC_DEPS EditGeometry JSON libraw::raw_r opencv_core
)

# GPU-agnostic DAG runtime (parameter arena, texture LRU, KV cache). No CUDA headers.
def_library(EditRuntime
    SOURCES
        ${ALCEDO_SRC_ROOT}/edit/runtime/edit_runtime.cpp
        ${ALCEDO_SRC_ROOT}/edit/runtime/graph_compiler.cpp
    PUBLIC_DEPS EditGraph EditGeometry
)

if(ALCEDO_CUDA_ENABLED)
    def_cuda_library(EditRuntimeCuda
        SOURCES
            ${ALCEDO_SRC_ROOT}/edit/runtime/cuda/cuda_backend.cpp
            ${ALCEDO_SRC_ROOT}/edit/runtime/cuda/cuda_develop_pass.cpp
            ${ALCEDO_SRC_ROOT}/edit/runtime/cuda/geometry_resample.cu
        PUBLIC_DEPS EditRuntime CUDA::cudart
        PRIVATE_DEPS EditInput RawProcessorOp
    )
    target_compile_definitions(EditRuntimeCuda PUBLIC HAVE_CUDA)
endif()
