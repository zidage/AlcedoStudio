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
