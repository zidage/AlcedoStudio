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
    PUBLIC_DEPS JSON
)
