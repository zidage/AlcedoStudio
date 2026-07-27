# Windows lensfun/duckdb runtime fixup for every EXECUTABLE in this directory.
include("${ALCEDO_TEST_ROOT}/cmake/AlcedoTestRegistration.cmake")

# Macro to reduce boilerplate for UI test targets.
macro(def_ui_test TARGET_NAME SOURCE_FILE)
    add_executable(${TARGET_NAME}
        ${ALCEDO_TEST_ROOT}/ui/${SOURCE_FILE}
        ${ALCEDO_TEST_ROOT}/ui/ui_test_main.cpp
        ${ALCEDO_TEST_ROOT}/ui/editor_dialog_stub.cpp
    )
    target_include_directories(${TARGET_NAME}
        PUBLIC ${ALCEDO_INCLUDE_DIR}
        PRIVATE ${ALCEDO_TEST_DIR}
    )
    target_link_libraries(${TARGET_NAME}
        PRIVATE
            AlbumBackendLib
            GTest::gtest
            Qt6::Test
            Qt6::Core
    )
    include(GoogleTest)
    if(NOT "${ARGN}" STREQUAL "")
        gtest_discover_tests(${TARGET_NAME} TEST_PREFIX "${TARGET_NAME}."
            PROPERTIES LABELS ${ARGN})
    else()
        gtest_discover_tests(${TARGET_NAME} TEST_PREFIX "${TARGET_NAME}.")
    endif()
endmacro()


# Editor session history owns Mini-Git working state and immutable captures.
# Link the production adapters directly (not AlbumBackendLib) so the binary does
# not pull the full QML/Qt shell graph into a unit-test process.
add_executable(EditorSessionHistoryPortTest
    ${ALCEDO_TEST_ROOT}/edit/history/editor_session_history_port_test.cpp
    ${CMAKE_SOURCE_DIR}/alcedo_studio/src/ui/alcedo_main/album_backend/editor_session_history_port.cpp
    ${CMAKE_SOURCE_DIR}/alcedo_studio/src/ui/alcedo_main/album_backend/editor_history_commit_presentation.cpp
    ${CMAKE_SOURCE_DIR}/alcedo_studio/src/ui/alcedo_main/album_backend/editor_session_pipeline_port.cpp)
target_include_directories(EditorSessionHistoryPortTest
  PUBLIC ${ALCEDO_INCLUDE_DIR} ${ALCEDO_TEST_DIR})
target_link_libraries(EditorSessionHistoryPortTest
  AdjustmentTransferService
  EditorAdjustmentPipeline
  EditorMiniGitMaterializer
  PipelineMgmtService
  EditHistory
  EditPipeline
  Operators
  GTest::gtest_main)
# Land next to the shared tests runtime DLLs (not the heavier ui/ shell DLL set).
set_target_properties(EditorSessionHistoryPortTest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/alcedo_studio/tests"
  RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/alcedo_studio/tests"
  RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/alcedo_studio/tests")
gtest_discover_tests(EditorSessionHistoryPortTest
  TEST_PREFIX "EditorSessionHistoryPortTest."
  PROPERTIES LABELS "editor_history;editor_session;ci_core_flow")
# Lands in the shared tests/ dir; still overwrite vcpkg applocal lensfun there.
alcedo_register_test_target(EditorSessionHistoryPortTest ui)

# Checkpoint storage owns Mini-Git materialization and recovery.
# Same direct-adapter link rule as history port: avoid AlbumBackendLib for unit runs.
add_executable(EditorSessionCheckpointStoreTest
    ${ALCEDO_TEST_ROOT}/edit/history/editor_session_checkpoint_store_test.cpp
    ${CMAKE_SOURCE_DIR}/alcedo_studio/src/ui/alcedo_main/album_backend/editor_session_checkpoint_store.cpp)
target_include_directories(EditorSessionCheckpointStoreTest
  PUBLIC ${ALCEDO_INCLUDE_DIR} ${ALCEDO_TEST_DIR})
target_link_libraries(EditorSessionCheckpointStoreTest
  EditorMiniGitMaterializer
  EditorSaveCheckpointCoordinator
  ProjectService
  EditHistory
  StorageService
  Operators
  GTest::gtest_main)
set_target_properties(EditorSessionCheckpointStoreTest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/alcedo_studio/tests"
  RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/alcedo_studio/tests"
  RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/alcedo_studio/tests")
gtest_discover_tests(EditorSessionCheckpointStoreTest
  TEST_PREFIX "EditorSessionCheckpointStoreTest."
  PROPERTIES LABELS "editor_history;editor_session;ci_core_flow")
alcedo_register_test_target(EditorSessionCheckpointStoreTest ui)

# Phase 6C-5: thumbnail invalidation port unit tests (no project/database needed).
add_executable(EditorSessionThumbnailPortTest
    ${ALCEDO_TEST_ROOT}/ui/editor_session_thumbnail_port_test.cpp)
target_include_directories(EditorSessionThumbnailPortTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(EditorSessionThumbnailPortTest
  AlbumBackendLib
  GTest::gtest_main)
gtest_discover_tests(EditorSessionThumbnailPortTest
  TEST_PREFIX "EditorSessionThumbnailPortTest."
  PROPERTIES LABELS "editor_session")
alcedo_register_test_target(EditorSessionThumbnailPortTest ui)

# Phase 6C-5: per-image journal writer port. Owns writer lookup, locks,
# append, and durability barrier. No database or QML dependency.
add_executable(EditorSessionJournalWriterPortTest
    ${ALCEDO_TEST_ROOT}/ui/editor_session_journal_writer_port_test.cpp)
target_include_directories(EditorSessionJournalWriterPortTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(EditorSessionJournalWriterPortTest
  AlbumBackendLib
  GTest::gtest_main)
gtest_discover_tests(EditorSessionJournalWriterPortTest
  TEST_PREFIX "EditorSessionJournalWriterPortTest."
  PROPERTIES LABELS "editor_session")
alcedo_register_test_target(EditorSessionJournalWriterPortTest ui)

# Pipeline guard and task publication ports own their state independently.
add_executable(EditorSessionPipelinePortTest
    ${ALCEDO_TEST_ROOT}/ui/editor_session_pipeline_port_test.cpp)
target_include_directories(EditorSessionPipelinePortTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(EditorSessionPipelinePortTest
  AlbumBackendLib
  EditHistory
  GTest::gtest_main)
gtest_discover_tests(EditorSessionPipelinePortTest
  TEST_PREFIX "EditorSessionPipelinePortTest."
  PROPERTIES LABELS "editor_session")
alcedo_register_test_target(EditorSessionPipelinePortTest ui)

add_executable(EditorSessionTaskPortTest
    ${ALCEDO_TEST_ROOT}/ui/editor_session_task_port_test.cpp)
target_include_directories(EditorSessionTaskPortTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(EditorSessionTaskPortTest
  AlbumBackendLib
  GTest::gtest_main)
gtest_discover_tests(EditorSessionTaskPortTest
  TEST_PREFIX "EditorSessionTaskPortTest."
  PROPERTIES LABELS "editor_session")
alcedo_register_test_target(EditorSessionTaskPortTest ui)

add_executable(EditorSessionRenderSchedulerPortTest
    ${ALCEDO_TEST_ROOT}/ui/editor_session_render_scheduler_port_test.cpp)
target_include_directories(EditorSessionRenderSchedulerPortTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(EditorSessionRenderSchedulerPortTest
  AlbumBackendLib
  GTest::gtest_main)
gtest_discover_tests(EditorSessionRenderSchedulerPortTest
  TEST_PREFIX "EditorSessionRenderSchedulerPortTest."
  PROPERTIES LABELS "editor_session")
alcedo_register_test_target(EditorSessionRenderSchedulerPortTest ui)

def_ui_test(AlbumBackendImportTest   album_backend_import_test.cpp)
alcedo_register_test_target(AlbumBackendImportTest ui)

def_ui_test(AlbumBackendProjectTest  album_backend_project_test.cpp)
alcedo_register_test_target(AlbumBackendProjectTest ui)

def_ui_test(AlbumBackendFolderTest   album_backend_folder_test.cpp)
alcedo_register_test_target(AlbumBackendFolderTest ui)

def_ui_test(AlbumBackendImageDeleteTest album_backend_image_delete_test.cpp)
alcedo_register_test_target(AlbumBackendImageDeleteTest ui)

def_ui_test(AlbumBackendImageDetailsTest album_backend_image_details_test.cpp)
alcedo_register_test_target(AlbumBackendImageDetailsTest ui)

def_ui_test(AlbumBackendRatingTest album_backend_rating_test.cpp "ci_raw_flow")
alcedo_register_test_target(AlbumBackendRatingTest ui)

def_ui_test(AlbumBackendThumbnailTest album_backend_thumbnail_test.cpp)
alcedo_register_test_target(AlbumBackendThumbnailTest ui)

def_ui_test(AlbumBackendI18nTest album_backend_i18n_test.cpp)
alcedo_register_test_target(AlbumBackendI18nTest ui)

def_ui_test(AlbumBackendCiWorkflowTest album_backend_ci_workflow_test.cpp "ci_raw_flow")
alcedo_register_test_target(AlbumBackendCiWorkflowTest ui)

def_ui_test(AlbumBackendBackgroundTaskTest album_backend_background_task_test.cpp)
alcedo_register_test_target(AlbumBackendBackgroundTaskTest ui)

def_ui_test(AlbumBackendInteractionPolicyTest album_backend_interaction_policy_test.cpp "interaction_policy")
alcedo_register_test_target(AlbumBackendInteractionPolicyTest ui)
def_ui_test(AlbumBackendDbWriteBarrierTest album_backend_db_write_barrier_test.cpp)
alcedo_register_test_target(AlbumBackendDbWriteBarrierTest ui)

def_ui_test(ApplicationModuleHostLifecycleTest application_module_host_lifecycle_test.cpp)
alcedo_register_test_target(ApplicationModuleHostLifecycleTest ui)

def_ui_test(ApplicationModuleHostShutdownTest application_module_host_shutdown_test.cpp)
alcedo_register_test_target(ApplicationModuleHostShutdownTest ui)

# Phase 0: deterministic editor RHI contracts (backend parse, fixtures, lease schema).
add_executable(EditorRhiContractsTest
    ${ALCEDO_TEST_ROOT}/ui/editor_rhi_contracts_test.cpp
)
target_include_directories(EditorRhiContractsTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(EditorRhiContractsTest PRIVATE
    EditorRhiContracts
    GTest::gtest_main
    Qt6::Core
)
gtest_discover_tests(EditorRhiContractsTest TEST_PREFIX "EditorRhiContractsTest.")
alcedo_register_test_target(EditorRhiContractsTest ui)

# Phase 0: maintained GPU presentation harness (manual / CI GPU workers).
if(TARGET EditorRhiHarness)
  if(WIN32 AND ALCEDO_CUDA_ENABLED)
    add_test(NAME EditorRhiHarness.CudaDirectPresentation
      COMMAND EditorRhiHarness --editor-backend=cuda --case=direct-presentation
    )
    set_tests_properties(EditorRhiHarness.CudaDirectPresentation PROPERTIES
      LABELS "gpu;editor_rhi;cuda"
      WORKING_DIRECTORY $<TARGET_FILE_DIR:EditorRhiHarness>
    )
  endif()
  if(WIN32 AND ALCEDO_OPENCL_ENABLED)
    add_test(NAME EditorRhiHarness.OpenClDirectPresentation
      COMMAND EditorRhiHarness --editor-backend=opencl --case=direct-presentation
    )
    set_tests_properties(EditorRhiHarness.OpenClDirectPresentation PROPERTIES
      LABELS "gpu;editor_rhi;opencl"
      WORKING_DIRECTORY $<TARGET_FILE_DIR:EditorRhiHarness>
    )
    add_test(NAME EditorRhiHarness.OpenClProductionLeasePresentation
      COMMAND EditorRhiHarness --editor-backend=opencl --case=production-lease-presentation
    )
    set_tests_properties(EditorRhiHarness.OpenClProductionLeasePresentation PROPERTIES
      LABELS "gpu;editor_rhi;opencl;production"
      WORKING_DIRECTORY $<TARGET_FILE_DIR:EditorRhiHarness>
    )
  endif()
  if(WIN32 AND ALCEDO_CUDA_ENABLED)
    add_test(NAME EditorRhiHarness.CudaProductionLeasePresentation
      COMMAND EditorRhiHarness --editor-backend=cuda --case=production-lease-presentation
    )
    set_tests_properties(EditorRhiHarness.CudaProductionLeasePresentation PROPERTIES
      LABELS "gpu;editor_rhi;cuda;production"
      WORKING_DIRECTORY $<TARGET_FILE_DIR:EditorRhiHarness>
    )
    add_test(NAME EditorRhiHarness.CudaProductionContinuousSubmit
      COMMAND EditorRhiHarness --editor-backend=cuda --case=production-continuous-submit
    )
    set_tests_properties(EditorRhiHarness.CudaProductionContinuousSubmit PROPERTIES
      LABELS "gpu;editor_rhi;cuda;production"
      WORKING_DIRECTORY $<TARGET_FILE_DIR:EditorRhiHarness>
    )
    add_test(NAME EditorRhiHarness.CudaProductionHideShow
      COMMAND EditorRhiHarness --editor-backend=cuda --case=production-hide-show
    )
    set_tests_properties(EditorRhiHarness.CudaProductionHideShow PROPERTIES
      LABELS "gpu;editor_rhi;cuda;production"
      WORKING_DIRECTORY $<TARGET_FILE_DIR:EditorRhiHarness>
    )
    add_test(NAME EditorRhiHarness.CudaProductionRendererRecreation
      COMMAND EditorRhiHarness --editor-backend=cuda --case=production-renderer-recreation
    )
    set_tests_properties(EditorRhiHarness.CudaProductionRendererRecreation PROPERTIES
      LABELS "gpu;editor_rhi;cuda;production"
      WORKING_DIRECTORY $<TARGET_FILE_DIR:EditorRhiHarness>
    )
  endif()
endif()

add_executable(GlobalSearchDialogQmlTest
    ${ALCEDO_TEST_ROOT}/ui/global_search_dialog_qml_test.cpp
    ${ALCEDO_TEST_ROOT}/ui/widget_test_main.cpp
    ${ALCEDO_TEST_ROOT}/ui/editor_dialog_stub.cpp
    ${ALCEDO_SRC_DIR}/ui/alcedo_main/app_theme.cpp
    ${ALCEDO_SRC_DIR}/include/ui/alcedo_main/app_theme.hpp
    ${ALCEDO_SRC_DIR}/config/resource.qrc
)
target_include_directories(GlobalSearchDialogQmlTest
    PUBLIC ${ALCEDO_INCLUDE_DIR}
    PRIVATE ${ALCEDO_TEST_DIR}
)
target_link_libraries(GlobalSearchDialogQmlTest
    PRIVATE
        AlbumBackendLib
        GTest::gtest
        Qt6::Core
        Qt6::Test
        Qt6::Qml
        Qt6::Quick
        Qt6::QuickControls2
        Qt6::QuickEffects
        Qt6::Svg
        Qt6::Widgets
)
target_compile_definitions(GlobalSearchDialogQmlTest
    PRIVATE ALCEDO_TEST_SRC_DIR="${ALCEDO_SRC_DIR}"
)
gtest_discover_tests(GlobalSearchDialogQmlTest TEST_PREFIX "GlobalSearchDialogQmlTest.")
alcedo_register_test_target(GlobalSearchDialogQmlTest ui)

add_executable(MainQmlWorkflowTest
    ${ALCEDO_TEST_ROOT}/ui/main_qml_workflow_test.cpp
    ${ALCEDO_TEST_ROOT}/ui/widget_test_main.cpp
    ${ALCEDO_TEST_ROOT}/ui/editor_dialog_stub.cpp
    ${ALCEDO_SRC_DIR}/ui/alcedo_main/app_theme.cpp
    ${ALCEDO_SRC_DIR}/include/ui/alcedo_main/app_theme.hpp
    ${ALCEDO_SRC_DIR}/config/resource.qrc
)
target_include_directories(MainQmlWorkflowTest
    PUBLIC ${ALCEDO_INCLUDE_DIR}
    PRIVATE ${ALCEDO_TEST_DIR}
)
target_link_libraries(MainQmlWorkflowTest
    PRIVATE
        AlbumBackendLib
        GTest::gtest
        Qt6::Core
        Qt6::Test
        Qt6::Qml
        Qt6::Quick
        Qt6::QuickControls2
        Qt6::QuickEffects
        Qt6::Svg
        Qt6::Widgets
)
target_compile_definitions(MainQmlWorkflowTest
    PRIVATE ALCEDO_TEST_SRC_DIR="${ALCEDO_SRC_DIR}"
)
gtest_discover_tests(MainQmlWorkflowTest TEST_PREFIX "MainQmlWorkflowTest.")
alcedo_register_test_target(MainQmlWorkflowTest ui)

# Phase 6C-5 Phase 6B: QML entry surfaces blocked during save and recovered after.
add_executable(EditorCheckpointQmlIntegrationTest
    ${ALCEDO_TEST_ROOT}/ui/editor_checkpoint_qml_integration_test.cpp
    ${ALCEDO_TEST_ROOT}/ui/main_qml_test_fixture.cpp
    ${ALCEDO_TEST_ROOT}/ui/widget_test_main.cpp
    ${ALCEDO_TEST_ROOT}/ui/editor_dialog_stub.cpp
    ${ALCEDO_SRC_DIR}/ui/alcedo_main/app_theme.cpp
    ${ALCEDO_SRC_DIR}/include/ui/alcedo_main/app_theme.hpp
    ${ALCEDO_SRC_DIR}/config/resource.qrc
)
target_include_directories(EditorCheckpointQmlIntegrationTest
    PUBLIC ${ALCEDO_INCLUDE_DIR}
    PRIVATE ${ALCEDO_TEST_DIR}
)
target_link_libraries(EditorCheckpointQmlIntegrationTest
    PRIVATE
        AlbumBackendLib
        GTest::gtest
        Qt6::Core
        Qt6::Test
        Qt6::Qml
        Qt6::Quick
        Qt6::QuickControls2
        Qt6::QuickEffects
        Qt6::Svg
        Qt6::Widgets
)
target_compile_definitions(EditorCheckpointQmlIntegrationTest
    PRIVATE ALCEDO_TEST_SRC_DIR="${ALCEDO_SRC_DIR}"
)
gtest_discover_tests(EditorCheckpointQmlIntegrationTest
    TEST_PREFIX "EditorCheckpointQmlIntegrationTest."
    PROPERTIES LABELS "workspace_qml;interaction_policy")
alcedo_register_test_target(EditorCheckpointQmlIntegrationTest ui)
add_executable(WorkspaceShellTest
    ${ALCEDO_TEST_ROOT}/ui/workspace_shell_test.cpp
    ${ALCEDO_TEST_ROOT}/ui/main_qml_test_fixture.cpp
    ${ALCEDO_TEST_ROOT}/ui/widget_test_main.cpp
    ${ALCEDO_TEST_ROOT}/ui/editor_dialog_stub.cpp
    ${ALCEDO_SRC_DIR}/ui/alcedo_main/app_theme.cpp
    ${ALCEDO_SRC_DIR}/include/ui/alcedo_main/app_theme.hpp
    ${ALCEDO_SRC_DIR}/config/resource.qrc
)
target_include_directories(WorkspaceShellTest
    PUBLIC ${ALCEDO_INCLUDE_DIR}
    PRIVATE ${ALCEDO_TEST_DIR}
)
target_link_libraries(WorkspaceShellTest
    PRIVATE
        AlbumBackendLib
        GTest::gtest
        Qt6::Core
        Qt6::Test
        Qt6::Qml
        Qt6::Quick
        Qt6::QuickControls2
        Qt6::QuickEffects
        Qt6::Svg
        Qt6::Widgets
)
target_compile_definitions(WorkspaceShellTest
    PRIVATE ALCEDO_TEST_SRC_DIR="${ALCEDO_SRC_DIR}"
)
gtest_discover_tests(WorkspaceShellTest TEST_PREFIX "WorkspaceShellTest."
  PROPERTIES LABELS "workspace_qml")
alcedo_register_test_target(WorkspaceShellTest ui)

# Phase 6A: typed adjustment models (no GPU/QML rendering; QSignalSpy drives the
# debounce timer deterministically via the event loop).
add_executable(EditorAdjustmentModelTest
    ${ALCEDO_TEST_ROOT}/ui/editor_adjustment_model_test.cpp
    ${ALCEDO_TEST_ROOT}/ui/ui_test_main.cpp
    ${ALCEDO_TEST_ROOT}/ui/editor_dialog_stub.cpp
)
target_include_directories(EditorAdjustmentModelTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(EditorAdjustmentModelTest
  PRIVATE
    AlbumBackendLib
    EditorSessionService
    GTest::gtest
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::Test
    Qt6::Widgets
)
gtest_discover_tests(EditorAdjustmentModelTest TEST_PREFIX "EditorAdjustmentModelTest.")
alcedo_register_test_target(EditorAdjustmentModelTest ui)

# Phase 6A: shared QML adjustment controls. Loads the controls from source via
# an inline harness; a RecordingSubmitter stands in for editorSession.
add_executable(EditorAdjustmentControlQmlTest
    ${ALCEDO_TEST_ROOT}/ui/editor_adjustment_control_qml_test.cpp
    ${ALCEDO_TEST_ROOT}/ui/widget_test_main.cpp
    ${ALCEDO_TEST_ROOT}/ui/editor_dialog_stub.cpp
    ${ALCEDO_SRC_DIR}/ui/alcedo_main/app_theme.cpp
    ${ALCEDO_SRC_DIR}/include/ui/alcedo_main/app_theme.hpp
    ${ALCEDO_SRC_DIR}/config/resource.qrc
)
target_include_directories(EditorAdjustmentControlQmlTest
    PUBLIC ${ALCEDO_INCLUDE_DIR}
    PRIVATE ${ALCEDO_TEST_DIR}
)
target_link_libraries(EditorAdjustmentControlQmlTest
    PRIVATE
        AlbumBackendLib
        GTest::gtest
        Qt6::Core
        Qt6::Test
        Qt6::Qml
        Qt6::Quick
        Qt6::QuickControls2
        Qt6::QuickEffects
        Qt6::Svg
        Qt6::Widgets
)
target_compile_definitions(EditorAdjustmentControlQmlTest
    PRIVATE ALCEDO_TEST_SRC_DIR="${ALCEDO_SRC_DIR}"
)
gtest_discover_tests(EditorAdjustmentControlQmlTest TEST_PREFIX "EditorAdjustmentControlQmlTest.")
alcedo_register_test_target(EditorAdjustmentControlQmlTest ui)

# Phase 7A / Phase 6: Versions panel (inline draft, outline selection, trash).
set(ALCEDO_HISTORY_VERSIONS_RAIL_QML_HARNESS
    ${ALCEDO_TEST_ROOT}/ui/editor_history_versions_rail_qml_harness.cpp
    ${ALCEDO_TEST_ROOT}/ui/editor_history_versions_rail_qml_harness.hpp
    ${ALCEDO_TEST_ROOT}/ui/widget_test_main.cpp
    ${ALCEDO_TEST_ROOT}/ui/editor_dialog_stub.cpp
    ${ALCEDO_SRC_DIR}/ui/alcedo_main/app_theme.cpp
    ${ALCEDO_SRC_DIR}/include/ui/alcedo_main/app_theme.hpp
    ${ALCEDO_SRC_DIR}/config/resource.qrc
)

add_executable(EditorVersionsPanelQmlTest
    ${ALCEDO_TEST_ROOT}/ui/editor_versions_panel_qml_test.cpp
    ${ALCEDO_HISTORY_VERSIONS_RAIL_QML_HARNESS}
)
target_include_directories(EditorVersionsPanelQmlTest
    PUBLIC ${ALCEDO_INCLUDE_DIR}
    PRIVATE ${ALCEDO_TEST_DIR}
    PRIVATE ${ALCEDO_TEST_ROOT}/ui
)
target_link_libraries(EditorVersionsPanelQmlTest
    PRIVATE
        AlbumBackendLib
        GTest::gtest
        Qt6::Core
        Qt6::Test
        Qt6::Qml
        Qt6::Quick
        Qt6::QuickControls2
        Qt6::QuickEffects
        Qt6::Svg
        Qt6::Widgets
)
target_compile_definitions(EditorVersionsPanelQmlTest
    PRIVATE ALCEDO_TEST_SRC_DIR="${ALCEDO_SRC_DIR}"
)
gtest_discover_tests(EditorVersionsPanelQmlTest
    TEST_PREFIX "EditorVersionsPanelQmlTest."
    PROPERTIES LABELS "workspace_qml;editor_history;editor_versions")
alcedo_register_test_target(EditorVersionsPanelQmlTest ui)

# Phase 7A / Phase 6: History transactions panel (toolbar, Branch Here, recovery).
add_executable(EditorHistoryTransactionsPanelQmlTest
    ${ALCEDO_TEST_ROOT}/ui/editor_history_transactions_panel_qml_test.cpp
    ${ALCEDO_HISTORY_VERSIONS_RAIL_QML_HARNESS}
)
target_include_directories(EditorHistoryTransactionsPanelQmlTest
    PUBLIC ${ALCEDO_INCLUDE_DIR}
    PRIVATE ${ALCEDO_TEST_DIR}
    PRIVATE ${ALCEDO_TEST_ROOT}/ui
)
target_link_libraries(EditorHistoryTransactionsPanelQmlTest
    PRIVATE
        AlbumBackendLib
        GTest::gtest
        Qt6::Core
        Qt6::Test
        Qt6::Qml
        Qt6::Quick
        Qt6::QuickControls2
        Qt6::QuickEffects
        Qt6::Svg
        Qt6::Widgets
)
target_compile_definitions(EditorHistoryTransactionsPanelQmlTest
    PRIVATE ALCEDO_TEST_SRC_DIR="${ALCEDO_SRC_DIR}"
)
gtest_discover_tests(EditorHistoryTransactionsPanelQmlTest
    TEST_PREFIX "EditorHistoryTransactionsPanelQmlTest."
    PROPERTIES LABELS "workspace_qml;editor_history")
alcedo_register_test_target(EditorHistoryTransactionsPanelQmlTest ui)

# Phase 6C-7: an already-restored snapshot must populate the QML Tone panel
# during its first editor binding, without emitting a new adjustment patch.
add_executable(EditorAdjustmentSnapshotQmlTest
    ${ALCEDO_TEST_ROOT}/ui/editor_adjustment_snapshot_qml_test.cpp
    ${ALCEDO_TEST_ROOT}/ui/widget_test_main.cpp
    ${ALCEDO_TEST_ROOT}/ui/editor_dialog_stub.cpp
    ${ALCEDO_SRC_DIR}/ui/alcedo_main/app_theme.cpp
    ${ALCEDO_SRC_DIR}/include/ui/alcedo_main/app_theme.hpp
    ${ALCEDO_SRC_DIR}/config/resource.qrc
)
target_include_directories(EditorAdjustmentSnapshotQmlTest
    PUBLIC ${ALCEDO_INCLUDE_DIR}
    PRIVATE ${ALCEDO_TEST_DIR}
)
target_link_libraries(EditorAdjustmentSnapshotQmlTest
    PRIVATE
        AlbumBackendLib
        GTest::gtest
        Qt6::Core
        Qt6::Test
        Qt6::Qml
        Qt6::Quick
        Qt6::QuickControls2
        Qt6::QuickEffects
        Qt6::Svg
        Qt6::Widgets
)
target_compile_definitions(EditorAdjustmentSnapshotQmlTest
    PRIVATE ALCEDO_TEST_SRC_DIR="${ALCEDO_SRC_DIR}"
)
gtest_discover_tests(EditorAdjustmentSnapshotQmlTest
    TEST_PREFIX "EditorAdjustmentSnapshotQmlTest.")
alcedo_register_test_target(EditorAdjustmentSnapshotQmlTest ui)

# Display Transform QML snapshot: verifies ODT params populate the
# Display Transform panel on first bind and method/space/EOTF changes.
add_executable(EditorDisplayTransformSnapshotQmlTest
    ${ALCEDO_TEST_ROOT}/ui/editor_display_transform_snapshot_qml_test.cpp
    ${ALCEDO_TEST_ROOT}/ui/widget_test_main.cpp
    ${ALCEDO_TEST_ROOT}/ui/editor_dialog_stub.cpp
    ${ALCEDO_SRC_DIR}/ui/alcedo_main/app_theme.cpp
    ${ALCEDO_SRC_DIR}/include/ui/alcedo_main/app_theme.hpp
    ${ALCEDO_SRC_DIR}/config/resource.qrc
)
target_include_directories(EditorDisplayTransformSnapshotQmlTest
    PUBLIC ${ALCEDO_INCLUDE_DIR}
    PRIVATE ${ALCEDO_TEST_DIR}
)
target_link_libraries(EditorDisplayTransformSnapshotQmlTest
    PRIVATE
        AlbumBackendLib
        GTest::gtest
        Qt6::Core
        Qt6::Test
        Qt6::Qml
        Qt6::Quick
        Qt6::QuickControls2
        Qt6::QuickEffects
        Qt6::Svg
        Qt6::Widgets
)
target_compile_definitions(EditorDisplayTransformSnapshotQmlTest
    PRIVATE ALCEDO_TEST_SRC_DIR="${ALCEDO_SRC_DIR}"
)
gtest_discover_tests(EditorDisplayTransformSnapshotQmlTest
    TEST_PREFIX "EditorDisplayTransformSnapshotQmlTest.")
alcedo_register_test_target(EditorDisplayTransformSnapshotQmlTest ui)

# Phase 6F: shared crop equations and QML-facing lens catalog adapter.
add_executable(EditorGeometryMathTest
    ${ALCEDO_TEST_ROOT}/ui/editor_geometry_math_test.cpp
    ${ALCEDO_TEST_ROOT}/ui/ui_test_main.cpp
    ${ALCEDO_TEST_ROOT}/ui/editor_dialog_stub.cpp
)
target_include_directories(EditorGeometryMathTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(EditorGeometryMathTest
  PRIVATE
    AlbumBackendLib
    GTest::gtest
    JSON
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::Test
    Qt6::Widgets
)
gtest_discover_tests(EditorGeometryMathTest TEST_PREFIX "EditorGeometryMathTest.")
alcedo_register_test_target(EditorGeometryMathTest ui)

# Phase 6F: Geometry panel snapshot, overlay synchronization, and patch shape.
add_executable(EditorGeometryPanelQmlTest
    ${ALCEDO_TEST_ROOT}/ui/editor_geometry_panel_qml_test.cpp
    ${ALCEDO_TEST_ROOT}/ui/widget_test_main.cpp
    ${ALCEDO_TEST_ROOT}/ui/editor_dialog_stub.cpp
    ${ALCEDO_SRC_DIR}/ui/alcedo_main/app_theme.cpp
    ${ALCEDO_SRC_DIR}/include/ui/alcedo_main/app_theme.hpp
    ${ALCEDO_SRC_DIR}/config/resource.qrc
)
target_include_directories(EditorGeometryPanelQmlTest
    PUBLIC ${ALCEDO_INCLUDE_DIR}
    PRIVATE ${ALCEDO_TEST_DIR}
)
target_link_libraries(EditorGeometryPanelQmlTest
    PRIVATE
        AlbumBackendLib
        GTest::gtest
        Qt6::Core
        Qt6::Test
        Qt6::Qml
        Qt6::Quick
        Qt6::QuickControls2
        Qt6::QuickEffects
        Qt6::Svg
        Qt6::Widgets
)
target_compile_definitions(EditorGeometryPanelQmlTest
    PRIVATE ALCEDO_TEST_SRC_DIR="${ALCEDO_SRC_DIR}"
)
gtest_discover_tests(EditorGeometryPanelQmlTest
    TEST_PREFIX "EditorGeometryPanelQmlTest."
    PROPERTIES LABELS "workspace_qml;geometry_panel")
alcedo_register_test_target(EditorGeometryPanelQmlTest ui)

# Phase 6G: RAW Decode capability gating, user input, complete operator params,
# and session snapshot replay through the production QML panel.
add_executable(EditorRawDecodePanelQmlTest
    ${ALCEDO_TEST_ROOT}/ui/editor_raw_decode_panel_qml_test.cpp
    ${ALCEDO_TEST_ROOT}/ui/widget_test_main.cpp
    ${ALCEDO_TEST_ROOT}/ui/editor_dialog_stub.cpp
    ${ALCEDO_SRC_DIR}/ui/alcedo_main/app_theme.cpp
    ${ALCEDO_SRC_DIR}/include/ui/alcedo_main/app_theme.hpp
    ${ALCEDO_SRC_DIR}/config/resource.qrc
)
target_include_directories(EditorRawDecodePanelQmlTest
    PUBLIC ${ALCEDO_INCLUDE_DIR}
    PRIVATE ${ALCEDO_TEST_DIR}
)
target_link_libraries(EditorRawDecodePanelQmlTest
    PRIVATE
        AlbumBackendLib
        GTest::gtest
        Qt6::Core
        Qt6::Test
        Qt6::Qml
        Qt6::Quick
        Qt6::QuickControls2
        Qt6::QuickEffects
        Qt6::Svg
        Qt6::Widgets
)
target_compile_definitions(EditorRawDecodePanelQmlTest
    PRIVATE ALCEDO_TEST_SRC_DIR="${ALCEDO_SRC_DIR}"
)
gtest_discover_tests(EditorRawDecodePanelQmlTest
    TEST_PREFIX "EditorRawDecodePanelQmlTest."
    PROPERTIES LABELS "workspace_qml;raw_decode_panel")
alcedo_register_test_target(EditorRawDecodePanelQmlTest ui)

# Geometry overlay draft: crop-frame edits must not emit CropRotate view routing.
add_executable(EditorGeometryOverlayDraftRoutingTest
    ${ALCEDO_TEST_ROOT}/ui/editor_geometry_overlay_draft_routing_test.cpp
    ${ALCEDO_TEST_ROOT}/ui/widget_test_main.cpp
)
target_include_directories(EditorGeometryOverlayDraftRoutingTest
    PUBLIC ${ALCEDO_INCLUDE_DIR}
)
target_link_libraries(EditorGeometryOverlayDraftRoutingTest
    PRIVATE
        EditorRhiViewport
        GTest::gtest
        Qt6::Core
        Qt6::Gui
        Qt6::Test
)
gtest_discover_tests(EditorGeometryOverlayDraftRoutingTest
    TEST_PREFIX "EditorGeometryOverlayDraftRoutingTest."
    PROPERTIES LABELS "geometry_overlay;interaction")
alcedo_register_test_target(EditorGeometryOverlayDraftRoutingTest ui)


# Real-project regression: replay production Main.qml Copy/Paste and verify
# that a reopened target publishes the saved Tone values to its QML models.
add_executable(EditorAdjustmentTransferRealProjectE2eTest
    ${ALCEDO_TEST_ROOT}/ui/editor_adjustment_transfer_real_project_e2e_test.cpp
    ${ALCEDO_TEST_ROOT}/ui/main_qml_test_fixture.cpp
    ${ALCEDO_TEST_ROOT}/ui/widget_test_main.cpp
    ${ALCEDO_TEST_ROOT}/ui/editor_dialog_stub.cpp
    ${ALCEDO_SRC_DIR}/ui/alcedo_main/app_theme.cpp
    ${ALCEDO_SRC_DIR}/include/ui/alcedo_main/app_theme.hpp
    ${ALCEDO_SRC_DIR}/config/resource.qrc
)
target_include_directories(EditorAdjustmentTransferRealProjectE2eTest
    PUBLIC ${ALCEDO_INCLUDE_DIR}
    PRIVATE ${ALCEDO_TEST_DIR}
)
target_link_libraries(EditorAdjustmentTransferRealProjectE2eTest
    PRIVATE
        AlbumBackendLib
        GTest::gtest
        Qt6::Core
        Qt6::Test
        Qt6::Qml
        Qt6::Quick
        Qt6::QuickControls2
        Qt6::QuickEffects
        Qt6::Svg
        Qt6::Widgets
)
target_compile_definitions(EditorAdjustmentTransferRealProjectE2eTest
    PRIVATE ALCEDO_TEST_SRC_DIR="${ALCEDO_SRC_DIR}"
)
gtest_discover_tests(EditorAdjustmentTransferRealProjectE2eTest
    TEST_PREFIX "EditorAdjustmentTransferRealProjectE2eTest."
    PROPERTIES LABELS "workspace_qml;real_project")
alcedo_register_test_target(EditorAdjustmentTransferRealProjectE2eTest ui)

# Phase 1D: extracted editor action components. Each target loads one QML
# component from source with only explicit fake collaborators.
add_executable(EditorWorkspaceNavigationQmlTest
    ${ALCEDO_TEST_ROOT}/ui/editor_workspace_navigation_qml_test.cpp
    ${ALCEDO_TEST_ROOT}/ui/widget_test_main.cpp
    ${ALCEDO_SRC_DIR}/ui/alcedo_main/app_theme.cpp
    ${ALCEDO_SRC_DIR}/include/ui/alcedo_main/app_theme.hpp
    ${ALCEDO_SRC_DIR}/config/resource.qrc
)
target_include_directories(EditorWorkspaceNavigationQmlTest
    PUBLIC ${ALCEDO_INCLUDE_DIR}
    PRIVATE ${ALCEDO_TEST_DIR}
)
target_link_libraries(EditorWorkspaceNavigationQmlTest
    PRIVATE
        GTest::gtest
        Qt6::Core
        Qt6::Test
        Qt6::Qml
        Qt6::Quick
        Qt6::QuickControls2
        Qt6::Svg
        Qt6::Widgets
)
target_compile_definitions(EditorWorkspaceNavigationQmlTest
    PRIVATE ALCEDO_TEST_SRC_DIR="${ALCEDO_SRC_DIR}"
)
gtest_discover_tests(EditorWorkspaceNavigationQmlTest
    TEST_PREFIX "EditorWorkspaceNavigationQmlTest."
    PROPERTIES LABELS "workspace_qml"
)
alcedo_register_test_target(EditorWorkspaceNavigationQmlTest ui)

add_executable(EditorAdjustmentTransferActionsQmlTest
    ${ALCEDO_TEST_ROOT}/ui/editor_adjustment_transfer_actions_qml_test.cpp
    ${ALCEDO_TEST_ROOT}/ui/widget_test_main.cpp
    ${ALCEDO_SRC_DIR}/ui/alcedo_main/app_theme.cpp
    ${ALCEDO_SRC_DIR}/include/ui/alcedo_main/app_theme.hpp
    ${ALCEDO_SRC_DIR}/config/resource.qrc
)
target_include_directories(EditorAdjustmentTransferActionsQmlTest
    PUBLIC ${ALCEDO_INCLUDE_DIR}
    PRIVATE ${ALCEDO_TEST_DIR}
)
target_link_libraries(EditorAdjustmentTransferActionsQmlTest
    PRIVATE
        GTest::gtest
        Qt6::Core
        Qt6::Test
        Qt6::Qml
        Qt6::Quick
        Qt6::QuickControls2
        Qt6::Svg
        Qt6::Widgets
)
target_compile_definitions(EditorAdjustmentTransferActionsQmlTest
    PRIVATE ALCEDO_TEST_SRC_DIR="${ALCEDO_SRC_DIR}"
)
gtest_discover_tests(EditorAdjustmentTransferActionsQmlTest
    TEST_PREFIX "EditorAdjustmentTransferActionsQmlTest."
    PROPERTIES LABELS "workspace_qml"
)
alcedo_register_test_target(EditorAdjustmentTransferActionsQmlTest ui)

# Phase 6B: tone-curve model (pointer-drag behavior + operator params JSON).
add_executable(EditorToneCurveModelTest
    ${ALCEDO_TEST_ROOT}/ui/editor_tone_curve_model_test.cpp
    ${ALCEDO_TEST_ROOT}/ui/ui_test_main.cpp
    ${ALCEDO_TEST_ROOT}/ui/editor_dialog_stub.cpp
)
target_include_directories(EditorToneCurveModelTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(EditorToneCurveModelTest
  PRIVATE
    AlbumBackendLib
    GTest::gtest
    JSON
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::Test
    Qt6::Widgets
)
gtest_discover_tests(EditorToneCurveModelTest TEST_PREFIX "EditorToneCurveModelTest.")
alcedo_register_test_target(EditorToneCurveModelTest ui)

# Phase 6B: pure tone-curve scene geometry (no QQuickWindow / GPU).
add_executable(EditorToneCurveGeometryTest
    ${ALCEDO_TEST_ROOT}/ui/editor_tone_curve_geometry_test.cpp
    ${ALCEDO_TEST_ROOT}/ui/ui_test_main.cpp
    ${ALCEDO_TEST_ROOT}/ui/editor_dialog_stub.cpp
)
target_include_directories(EditorToneCurveGeometryTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(EditorToneCurveGeometryTest
  PRIVATE
    AlbumBackendLib
    GTest::gtest
    JSON
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::Test
    Qt6::Widgets
)
gtest_discover_tests(EditorToneCurveGeometryTest TEST_PREFIX "EditorToneCurveGeometryTest.")
alcedo_register_test_target(EditorToneCurveGeometryTest ui)

# Phase 6D: Look panel typed models (WB / HSL / CDL / LUT) — no QML/GPU.
add_executable(EditorLookModelTest
    ${ALCEDO_TEST_ROOT}/ui/editor_look_model_test.cpp
    ${ALCEDO_TEST_ROOT}/ui/ui_test_main.cpp
    ${ALCEDO_TEST_ROOT}/ui/editor_dialog_stub.cpp
)
target_include_directories(EditorLookModelTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(EditorLookModelTest
  PRIVATE
    AlbumBackendLib
    GTest::gtest
    JSON
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::Test
    Qt6::Widgets
)
gtest_discover_tests(EditorLookModelTest TEST_PREFIX "EditorLookModelTest.")
alcedo_register_test_target(EditorLookModelTest ui)

# Phase 6D: pure CDL trackball disc geometry (no QQuickWindow / GPU).
add_executable(EditorCdlTrackballGeometryTest
    ${ALCEDO_TEST_ROOT}/ui/editor_cdl_trackball_geometry_test.cpp
    ${ALCEDO_TEST_ROOT}/ui/ui_test_main.cpp
    ${ALCEDO_TEST_ROOT}/ui/editor_dialog_stub.cpp
)
target_include_directories(EditorCdlTrackballGeometryTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(EditorCdlTrackballGeometryTest
  PRIVATE
    AlbumBackendLib
    GTest::gtest
    JSON
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::Test
    Qt6::Widgets
)
gtest_discover_tests(EditorCdlTrackballGeometryTest TEST_PREFIX "EditorCdlTrackballGeometryTest.")
alcedo_register_test_target(EditorCdlTrackballGeometryTest ui)

# LUT panel QML: selection scroll contract, favorites, filter, appTheme VI tokens.
add_executable(EditorLutPanelQmlTest
    ${ALCEDO_TEST_ROOT}/ui/editor_lut_panel_qml_test.cpp
    ${ALCEDO_TEST_ROOT}/ui/widget_test_main.cpp
    ${ALCEDO_SRC_DIR}/ui/alcedo_main/app_theme.cpp
    ${ALCEDO_SRC_DIR}/include/ui/alcedo_main/app_theme.hpp
    ${ALCEDO_SRC_DIR}/config/resource.qrc
)
target_include_directories(EditorLutPanelQmlTest
    PUBLIC ${ALCEDO_INCLUDE_DIR}
    PRIVATE ${ALCEDO_TEST_DIR}
)
target_link_libraries(EditorLutPanelQmlTest
    PRIVATE
        GTest::gtest
        Qt6::Core
        Qt6::Test
        Qt6::Qml
        Qt6::Quick
        Qt6::QuickControls2
        Qt6::Svg
        Qt6::Widgets
)
target_compile_definitions(EditorLutPanelQmlTest
    PRIVATE ALCEDO_TEST_SRC_DIR="${ALCEDO_SRC_DIR}"
)
set_target_properties(EditorLutPanelQmlTest PROPERTIES AUTOMOC ON)
gtest_discover_tests(EditorLutPanelQmlTest
    TEST_PREFIX "EditorLutPanelQmlTest."
    PROPERTIES LABELS "workspace_qml;lut_panel"
)
alcedo_register_test_target(EditorLutPanelQmlTest ui)

# Phase 6D Look panel interaction reproduction (layout, drag, double-click, flick).
add_executable(EditorLookPanelInteractionTest
    ${ALCEDO_TEST_ROOT}/ui/editor_look_panel_interaction_test.cpp
    ${ALCEDO_TEST_ROOT}/ui/widget_test_main.cpp
    ${ALCEDO_TEST_ROOT}/ui/editor_dialog_stub.cpp
    ${ALCEDO_SRC_DIR}/ui/alcedo_main/app_theme.cpp
    ${ALCEDO_SRC_DIR}/include/ui/alcedo_main/app_theme.hpp
    ${ALCEDO_SRC_DIR}/config/resource.qrc
)
target_include_directories(EditorLookPanelInteractionTest
    PUBLIC ${ALCEDO_INCLUDE_DIR}
    PRIVATE ${ALCEDO_TEST_DIR}
)
target_link_libraries(EditorLookPanelInteractionTest
    PRIVATE
        AlbumBackendLib
        GTest::gtest
        Qt6::Core
        Qt6::Test
        Qt6::Qml
        Qt6::Quick
        Qt6::QuickControls2
        Qt6::QuickEffects
        Qt6::Svg
        Qt6::Widgets
)
target_compile_definitions(EditorLookPanelInteractionTest
    PRIVATE ALCEDO_TEST_SRC_DIR="${ALCEDO_SRC_DIR}"
)
gtest_discover_tests(EditorLookPanelInteractionTest
    TEST_PREFIX "EditorLookPanelInteractionTest."
    PROPERTIES LABELS "workspace_qml;look_panel"
)
alcedo_register_test_target(EditorLookPanelInteractionTest ui)

# Qt Quick Test: multi-slider user ops through production session/history path.
# QML under tests/ui/qml/tst_*.qml; C++ wires EditorSessionController + real
# EditorSessionHistoryPort + pipeline guard + contended render worker.
add_executable(EditorMultiSliderQuickTest
    ${ALCEDO_TEST_ROOT}/ui/editor_multi_slider_quicktest.cpp
    ${ALCEDO_SRC_DIR}/ui/alcedo_main/app_theme.cpp
    ${ALCEDO_SRC_DIR}/include/ui/alcedo_main/app_theme.hpp
    ${ALCEDO_SRC_DIR}/config/resource.qrc
)
target_include_directories(EditorMultiSliderQuickTest
    PUBLIC ${ALCEDO_INCLUDE_DIR}
    PRIVATE ${ALCEDO_TEST_DIR}
)
target_link_libraries(EditorMultiSliderQuickTest
    PRIVATE
        AlbumBackendLib
        Operators
        EditPipeline
        EditHistory
        EditorAdjustmentPipeline
        Qt6::Core
        Qt6::Gui
        Qt6::Qml
        Qt6::Quick
        Qt6::QuickControls2
        Qt6::QuickTest
        Qt6::Test
        Qt6::Widgets
)
# QUICK_TEST_SOURCE_DIR → directory containing tst_*.qml
target_compile_definitions(EditorMultiSliderQuickTest
    PRIVATE
        ALCEDO_TEST_SRC_DIR="${ALCEDO_SRC_DIR}"
        QUICK_TEST_SOURCE_DIR="${ALCEDO_TEST_ROOT}/ui/qml"
)
# Quick Test discovers tests at runtime from QML; register one ctest entry.
add_test(NAME EditorMultiSliderQuickTest
    COMMAND EditorMultiSliderQuickTest
    WORKING_DIRECTORY $<TARGET_FILE_DIR:EditorMultiSliderQuickTest>
)
set_tests_properties(EditorMultiSliderQuickTest PROPERTIES
    LABELS "workspace_qml;look_panel;quicktest"
    TIMEOUT 600
)
alcedo_register_test_target(EditorMultiSliderQuickTest ui)

add_executable(EditorRealRawGpuE2eTest
    ${ALCEDO_TEST_ROOT}/ui/editor_real_raw_gpu_e2e_test.cpp
    ${ALCEDO_TEST_ROOT}/ui/editor_dialog_stub.cpp
    ${ALCEDO_SRC_DIR}/ui/alcedo_main/app_theme.cpp
    ${ALCEDO_SRC_DIR}/include/ui/alcedo_main/app_theme.hpp
    ${ALCEDO_SRC_DIR}/config/resource.qrc
)
target_include_directories(EditorRealRawGpuE2eTest
    PUBLIC ${ALCEDO_INCLUDE_DIR}
    PRIVATE ${ALCEDO_TEST_DIR}
)
target_link_libraries(EditorRealRawGpuE2eTest
    PRIVATE
        AlbumBackendLib
        GTest::gtest
        Qt6::Core
        Qt6::Test
        Qt6::Qml
        Qt6::Quick
        Qt6::QuickControls2
        Qt6::QuickEffects
        Qt6::Svg
        Qt6::Widgets
)
target_compile_definitions(EditorRealRawGpuE2eTest
    PRIVATE ALCEDO_TEST_SRC_DIR="${ALCEDO_SRC_DIR}"
)
alcedo_register_test_target(EditorRealRawGpuE2eTest ui)

if(WIN32 AND ALCEDO_CUDA_ENABLED)
  add_test(NAME EditorRealRawGpuE2eTest.CudaSustainedImageSwitches
    COMMAND ${CMAKE_COMMAND} -E env
      ALCEDO_TEST_EDITOR_BACKEND=cuda
      ALCEDO_REAL_RAW_E2E_SWITCHES=8
      $<TARGET_FILE:EditorRealRawGpuE2eTest>
  )
  set_tests_properties(EditorRealRawGpuE2eTest.CudaSustainedImageSwitches PROPERTIES
    LABELS "gpu;cuda;real_raw;phase5b"
    TIMEOUT 1800
    WORKING_DIRECTORY $<TARGET_FILE_DIR:EditorRealRawGpuE2eTest>
  )
endif()

if(WIN32 AND ALCEDO_OPENCL_ENABLED)
  add_test(NAME EditorRealRawGpuE2eTest.OpenClSustainedImageSwitches
    COMMAND ${CMAKE_COMMAND} -E env
      ALCEDO_TEST_EDITOR_BACKEND=opencl
      ALCEDO_REAL_RAW_E2E_SWITCHES=8
      $<TARGET_FILE:EditorRealRawGpuE2eTest>
  )
  set_tests_properties(EditorRealRawGpuE2eTest.OpenClSustainedImageSwitches PROPERTIES
    LABELS "gpu;opencl;real_raw;phase5b"
    TIMEOUT 1800
    WORKING_DIRECTORY $<TARGET_FILE_DIR:EditorRealRawGpuE2eTest>
  )
endif()

add_executable(LanguageManagerTest
    ${ALCEDO_TEST_ROOT}/ui/language_manager_test.cpp
    ${ALCEDO_TEST_ROOT}/ui/ui_test_main.cpp
)
target_include_directories(LanguageManagerTest
    PUBLIC ${ALCEDO_INCLUDE_DIR}
    PRIVATE ${ALCEDO_TEST_DIR}
)
target_link_libraries(LanguageManagerTest
    PRIVATE
        UiLocalization
        GTest::gtest
        Qt6::Test
        Qt6::Core
        Qt6::Qml
)
gtest_discover_tests(LanguageManagerTest TEST_PREFIX "LanguageManagerTest.")
alcedo_register_test_target(LanguageManagerTest ui)

# Phase 6C-5 Phase 6C: production-style A-to-B integration test.
add_executable(EditorCheckpointNavigationTest
    ${ALCEDO_TEST_ROOT}/integration/editor_checkpoint_navigation_test.cpp
    ${ALCEDO_TEST_ROOT}/ui/main_qml_test_fixture.cpp
    ${ALCEDO_TEST_ROOT}/ui/widget_test_main.cpp
    ${ALCEDO_TEST_ROOT}/ui/editor_dialog_stub.cpp
    ${ALCEDO_SRC_DIR}/ui/alcedo_main/app_theme.cpp
    ${ALCEDO_SRC_DIR}/include/ui/alcedo_main/app_theme.hpp
    ${ALCEDO_SRC_DIR}/config/resource.qrc
)
target_include_directories(EditorCheckpointNavigationTest
    PUBLIC ${ALCEDO_INCLUDE_DIR}
    PRIVATE ${ALCEDO_TEST_DIR}
)
target_link_libraries(EditorCheckpointNavigationTest
    PRIVATE
        AlbumBackendLib
        GTest::gtest
        Qt6::Core
        Qt6::Test
        Qt6::Qml
        Qt6::Quick
        Qt6::QuickControls2
        Qt6::QuickEffects
        Qt6::Svg
        Qt6::Widgets
)
target_compile_definitions(EditorCheckpointNavigationTest
    PRIVATE ALCEDO_TEST_SRC_DIR="${ALCEDO_SRC_DIR}"
)
gtest_discover_tests(EditorCheckpointNavigationTest
    TEST_PREFIX "EditorCheckpointNavigationTest."
    PROPERTIES LABELS "workspace_qml;interaction_policy;integration")
alcedo_register_test_target(EditorCheckpointNavigationTest ui)
