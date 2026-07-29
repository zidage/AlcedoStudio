# Application-service test target ownership.
set(ALCEDO_TEST_APP_ROOT "${ALCEDO_TEST_ROOT}/app")
include("${ALCEDO_TEST_ROOT}/cmake/AlcedoTestRegistration.cmake")

add_executable(EditorAdjustmentPipelineTest ${ALCEDO_TEST_ROOT}/app/editor_adjustment_pipeline_test.cpp)
target_include_directories(EditorAdjustmentPipelineTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(EditorAdjustmentPipelineTest
    PRIVATE GTest::gtest_main EditorAdjustmentPipeline Operators EditPipeline JSON)
gtest_discover_tests(EditorAdjustmentPipelineTest TEST_PREFIX "EditorAdjustmentPipelineTest.")
alcedo_register_test_target(EditorAdjustmentPipelineTest app)

# Geometry overlay preview: crop disable + bilinear GPU resize path that the
# unified scheduler runs when the Geometry panel owns the source-frame overlay.
add_executable(EditorGeometryOverlayPipelineTest
    ${ALCEDO_TEST_ROOT}/app/editor_geometry_overlay_pipeline_test.cpp)
target_include_directories(EditorGeometryOverlayPipelineTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(EditorGeometryOverlayPipelineTest
    PRIVATE GTest::gtest_main EditorAdjustmentPipeline Operators EditPipeline JSON
            ${ALCEDO_TEST_OPENCV_MIN_DEPS})
gtest_discover_tests(EditorGeometryOverlayPipelineTest
    TEST_PREFIX "EditorGeometryOverlayPipelineTest."
    PROPERTIES LABELS "app;geometry;gpu")
alcedo_register_test_target(EditorGeometryOverlayPipelineTest app)

add_executable(ImportServiceTest ${ALCEDO_TEST_ROOT}/app/import_service_test.cpp)
target_include_directories(ImportServiceTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(ImportServiceTest ImportService ProjectService GTest::gtest_main)

alcedo_register_test_target(ImportServiceTest app)

add_executable(SleeveServiceTest ${ALCEDO_TEST_ROOT}/app/sleeve_service_test.cpp)
target_include_directories(SleeveServiceTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(SleeveServiceTest
  ProjectService
  PipelineMgmtService
  EditHistoryMgmtService
  GTest::gtest_main)

alcedo_register_test_target(SleeveServiceTest app)

add_executable(FilterServiceTest ${ALCEDO_TEST_ROOT}/app/filter_service_test.cpp)
target_include_directories(FilterServiceTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(FilterServiceTest ImportService ProjectService SleeveFilterService GTest::gtest_main)
gtest_discover_tests(FilterServiceTest TEST_PREFIX "FilterServiceTest.")

alcedo_copy_duckdb_fts_extension(FilterServiceTest)
alcedo_register_test_target(FilterServiceTest app)

add_executable(SearchQueryClassifierTest ${ALCEDO_TEST_ROOT}/app/search_query_classifier_test.cpp)
target_include_directories(SearchQueryClassifierTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(SearchQueryClassifierTest SleeveFilterService GTest::gtest_main)
gtest_discover_tests(SearchQueryClassifierTest TEST_PREFIX "SearchQueryClassifierTest.")
alcedo_register_test_target(SearchQueryClassifierTest app)

add_executable(PipelineServiceTest ${ALCEDO_TEST_ROOT}/app/pipeline_service_test.cpp)
target_include_directories(PipelineServiceTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(PipelineServiceTest ProjectService PipelineMgmtService GTest::gtest_main)
gtest_discover_tests(PipelineServiceTest TEST_PREFIX "PipelineServiceTest."
  PROPERTIES LABELS "ci_core_flow")

alcedo_register_test_target(PipelineServiceTest app ci_core)

# Phase 6C-8: Mini-Git-based Paste and Merge tests using the project fixture.
add_executable(AdjustmentTransferServiceMiniGitTest
  ${ALCEDO_TEST_ROOT}/app/adjustment_transfer_service_mini_git_test.cpp)
target_include_directories(AdjustmentTransferServiceMiniGitTest
  PUBLIC ${ALCEDO_INCLUDE_DIR} ${ALCEDO_TEST_DIR})
target_link_libraries(AdjustmentTransferServiceMiniGitTest
  AdjustmentTransferService
  EditorSessionTestSupport
  ProjectService
  PipelineMgmtService
  EditHistoryMgmtService
  EditHistory
  GTest::gtest_main
)
gtest_discover_tests(AdjustmentTransferServiceMiniGitTest
  TEST_PREFIX "AdjustmentTransferServiceMiniGitTest."
  PROPERTIES LABELS "ci_core_flow;editor_session")
alcedo_register_test_target(AdjustmentTransferServiceMiniGitTest app ci_core)

add_executable(EditHistoryMgmtServiceTest ${ALCEDO_TEST_ROOT}/app/history_mgmt_service_test.cpp)
target_include_directories(EditHistoryMgmtServiceTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(EditHistoryMgmtServiceTest ProjectService EditHistoryMgmtService GTest::gtest_main)
gtest_discover_tests(EditHistoryMgmtServiceTest TEST_PREFIX "EditHistoryMgmtServiceTest."
  PROPERTIES LABELS "ci_core_flow")
alcedo_register_test_target(EditHistoryMgmtServiceTest app ci_core)

# Phase 5A: application-layer editor session + render coordinator (no GPU/QML).
# EditorSessionCommandQueue is linked explicitly because the service's public
# header exposes the queue types while linking the queue library privately.
add_executable(EditorRenderCoordinatorTest ${ALCEDO_TEST_ROOT}/app/editor_render_coordinator_test.cpp)
target_include_directories(EditorRenderCoordinatorTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(EditorRenderCoordinatorTest
  EditorSessionService EditorSessionCommandQueue GTest::gtest_main)
gtest_discover_tests(EditorRenderCoordinatorTest TEST_PREFIX "EditorRenderCoordinatorTest."
  PROPERTIES LABELS "ci_core_flow")
alcedo_register_test_target(EditorRenderCoordinatorTest app ci_core)

# Focused editor-session / Mini-Git test fixtures (no production behavior).
add_library(EditorSessionTestSupport STATIC
  ${ALCEDO_TEST_ROOT}/support/editor_mini_git_project_fixture.cpp
  ${ALCEDO_TEST_ROOT}/support/editor_save_checkpoint_fixture.cpp
  ${ALCEDO_TEST_ROOT}/support/editor_session_navigation_fixture.cpp
)
target_include_directories(EditorSessionTestSupport
  PUBLIC ${ALCEDO_INCLUDE_DIR} ${ALCEDO_TEST_DIR})
target_link_libraries(EditorSessionTestSupport
  PUBLIC
    EditorSaveCheckpointService
    EditorSessionLifecycle
    EditorSessionNavigationController
    EditorSessionEditController
    EditorSessionRenderController
    EditorMiniGitMaterializer
    ProjectService
    EditHistory
    StorageService
    GTest::gtest
)
alcedo_register_test_target(EditorSessionTestSupport app)

# EditorSaveCheckpointCoordinator owns the project-wide save lock only.
add_executable(EditorSaveCheckpointCoordinatorTest
  ${ALCEDO_TEST_ROOT}/app/editor_save_checkpoint_coordinator_test.cpp)
target_include_directories(EditorSaveCheckpointCoordinatorTest
  PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(EditorSaveCheckpointCoordinatorTest
  EditorSaveCheckpointCoordinator GTest::gtest_main)
gtest_discover_tests(EditorSaveCheckpointCoordinatorTest
  TEST_PREFIX "EditorSaveCheckpointCoordinatorTest."
  PROPERTIES LABELS "ci_core_flow;editor_session")
alcedo_register_test_target(EditorSaveCheckpointCoordinatorTest app ci_core)

# EditorSaveCheckpointService owns save work (task, journal
# commit, materialization). Focused unit tests with fake ports.
add_executable(EditorSaveCheckpointServiceTest ${ALCEDO_TEST_ROOT}/app/editor_save_checkpoint_service_test.cpp)
target_include_directories(EditorSaveCheckpointServiceTest
  PUBLIC ${ALCEDO_INCLUDE_DIR} ${ALCEDO_TEST_DIR})
target_link_libraries(EditorSaveCheckpointServiceTest
  EditorSessionTestSupport EditorSaveCheckpointService GTest::gtest_main)
gtest_discover_tests(EditorSaveCheckpointServiceTest TEST_PREFIX "EditorSaveCheckpointServiceTest."
  PROPERTIES LABELS "ci_core_flow;editor_session")
alcedo_register_test_target(EditorSaveCheckpointServiceTest app ci_core)

# EditorSessionLifecycle owns state, identity, guards, and
# error. Focused unit tests with fake ports.
add_executable(EditorSessionLifecycleTest ${ALCEDO_TEST_ROOT}/app/editor_session_lifecycle_test.cpp)
target_include_directories(EditorSessionLifecycleTest
  PUBLIC ${ALCEDO_INCLUDE_DIR} ${ALCEDO_TEST_DIR})
target_link_libraries(EditorSessionLifecycleTest EditorSessionLifecycle GTest::gtest_main
  xxHash::xxhash)
gtest_discover_tests(EditorSessionLifecycleTest TEST_PREFIX "EditorSessionLifecycleTest."
  PROPERTIES LABELS "ci_core_flow;editor_session")
alcedo_register_test_target(EditorSessionLifecycleTest app ci_core)

# EditorSessionNavigationController owns pending A-to-B/
# close actions. Focused unit tests with fake ports and a real save service.
add_executable(EditorSessionNavigationControllerTest
    ${ALCEDO_TEST_ROOT}/app/editor_session_navigation_controller_test.cpp)
target_include_directories(EditorSessionNavigationControllerTest
  PUBLIC ${ALCEDO_INCLUDE_DIR} ${ALCEDO_TEST_DIR})
target_link_libraries(EditorSessionNavigationControllerTest
    EditorSessionTestSupport
    EditorSessionNavigationController EditorSaveCheckpointService EditorSessionLifecycle
    GTest::gtest_main)
gtest_discover_tests(EditorSessionNavigationControllerTest
    TEST_PREFIX "EditorSessionNavigationControllerTest."
    PROPERTIES LABELS "ci_core_flow;editor_session")
alcedo_register_test_target(EditorSessionNavigationControllerTest app ci_core)

# EditorSessionRenderController owns render/first-frame
# state. Focused unit tests with a real coordinator and bootstrap scheduler.
add_executable(EditorSessionRenderControllerTest ${ALCEDO_TEST_ROOT}/app/editor_session_render_controller_test.cpp)
target_include_directories(EditorSessionRenderControllerTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(EditorSessionRenderControllerTest
    EditorSessionRenderController EditorSessionLifecycle EditorSessionService
    EditorSessionCommandQueue GTest::gtest_main)
gtest_discover_tests(EditorSessionRenderControllerTest
    TEST_PREFIX "EditorSessionRenderControllerTest."
    PROPERTIES LABELS "ci_core_flow;editor_session")
alcedo_register_test_target(EditorSessionRenderControllerTest app ci_core)

# EditorSessionEditController owns adjustment/history
# operations. Focused unit tests with fake ports.
add_executable(EditorSessionEditControllerTest ${ALCEDO_TEST_ROOT}/app/editor_session_edit_controller_test.cpp)
target_include_directories(EditorSessionEditControllerTest
  PUBLIC ${ALCEDO_INCLUDE_DIR} ${ALCEDO_TEST_DIR})
target_link_libraries(EditorSessionEditControllerTest
    EditorSessionEditController EditorSessionLifecycle EditHistory
    GTest::gtest_main)
gtest_discover_tests(EditorSessionEditControllerTest
    TEST_PREFIX "EditorSessionEditControllerTest."
    PROPERTIES LABELS "ci_core_flow;editor_session")
alcedo_register_test_target(EditorSessionEditControllerTest app ci_core)

# EditorSessionService facade routing tests. Limited to
# routing and externally visible result ordering; component behavior belongs in
# the five module test targets above.
add_executable(EditorSessionServiceFacadeTest ${ALCEDO_TEST_ROOT}/app/editor_session_service_facade_test.cpp)
target_include_directories(EditorSessionServiceFacadeTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(EditorSessionServiceFacadeTest
    EditorSessionService JSON xxHash ${ALCEDO_TEST_OPENCV_MIN_DEPS} GTest::gtest_main)
gtest_discover_tests(EditorSessionServiceFacadeTest
    TEST_PREFIX "EditorSessionServiceFacadeTest."
    PROPERTIES LABELS "ci_core_flow;editor_session")
alcedo_register_test_target(EditorSessionServiceFacadeTest app ci_core)

# CQ0: editor-session command-queue baseline (failing-evidence) tests. Drives
# the real EditorSessionService facade through controllable ports and the real
# EditorRenderCoordinator. The shared support header is header-only.
add_executable(EditorSessionCommandQueueBaselineTest
    ${ALCEDO_TEST_ROOT}/app/editor_session_command_queue_baseline_test.cpp)
target_include_directories(EditorSessionCommandQueueBaselineTest
    PUBLIC ${ALCEDO_INCLUDE_DIR} ${ALCEDO_TEST_DIR})
target_link_libraries(EditorSessionCommandQueueBaselineTest
    EditorSessionService xxHash JSON ${ALCEDO_TEST_OPENCV_MIN_DEPS} GTest::gtest_main)
gtest_discover_tests(EditorSessionCommandQueueBaselineTest
    TEST_PREFIX "EditorSessionCommandQueueBaselineTest."
    PROPERTIES LABELS "editor_session")
alcedo_register_test_target(EditorSessionCommandQueueBaselineTest app)

add_executable(EditorSessionControllerPhase5ATest
    ${ALCEDO_TEST_ROOT}/ui/editor_session_controller_phase5a_test.cpp
    ${ALCEDO_TEST_ROOT}/ui/ui_test_main.cpp
    ${ALCEDO_TEST_ROOT}/ui/editor_dialog_stub.cpp
)
target_include_directories(EditorSessionControllerPhase5ATest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_compile_definitions(EditorSessionControllerPhase5ATest PRIVATE
  ALCEDO_REPO_ROOT="${CMAKE_SOURCE_DIR}")
target_link_libraries(EditorSessionControllerPhase5ATest
  PRIVATE
    AlbumBackendLib
    EditorSessionService
    GTest::gtest
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::Widgets
)
gtest_discover_tests(EditorSessionControllerPhase5ATest
  TEST_PREFIX "EditorSessionControllerPhase5ATest."
  PROPERTIES LABELS "ci_core_flow")
alcedo_register_test_target(EditorSessionControllerPhase5ATest app ci_core)

# Phase 7A R4: focused history-operation correlator (no full session graph).
add_executable(EditorHistoryOperationPublisherTest
    ${ALCEDO_TEST_ROOT}/ui/editor_history_operation_publisher_test.cpp
    ${CMAKE_SOURCE_DIR}/alcedo_studio/src/ui/alcedo_main/album_backend/editor_history_operation_publisher.cpp
    ${ALCEDO_TEST_ROOT}/ui/ui_test_main.cpp
)
target_include_directories(EditorHistoryOperationPublisherTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(EditorHistoryOperationPublisherTest
  PRIVATE
    GTest::gtest
    Qt6::Core
)
gtest_discover_tests(EditorHistoryOperationPublisherTest
  TEST_PREFIX "EditorHistoryOperationPublisherTest."
  PROPERTIES LABELS "ci_core_flow;editor_history")
alcedo_register_test_target(EditorHistoryOperationPublisherTest app ci_core)

add_executable(ProjectServiceTest ${ALCEDO_TEST_ROOT}/app/project_service_test.cpp)
target_include_directories(ProjectServiceTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(ProjectServiceTest ProjectService GTest::gtest_main)
gtest_discover_tests(ProjectServiceTest TEST_PREFIX "ProjectServiceTest."
  PROPERTIES LABELS "ci_core_flow")
alcedo_register_test_target(ProjectServiceTest app ci_core)

add_executable(AiSidecarFakeRuntime ${ALCEDO_TEST_ROOT}/app/ai_sidecar_fake_runtime_main.cpp)
alcedo_register_test_target(AiSidecarFakeRuntime app)

add_executable(AiSidecarRuntimeServiceTest ${ALCEDO_TEST_ROOT}/app/ai_sidecar_runtime_service_test.cpp)
target_include_directories(AiSidecarRuntimeServiceTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_compile_definitions(AiSidecarRuntimeServiceTest PRIVATE
  ALCEDO_AI_SIDECAR_FAKE_RUNTIME_PATH="$<TARGET_FILE:AiSidecarFakeRuntime>")
target_link_libraries(AiSidecarRuntimeServiceTest AiSidecarRuntimeService ProjectService GTest::gtest_main)
add_dependencies(AiSidecarRuntimeServiceTest AiSidecarFakeRuntime)
gtest_discover_tests(AiSidecarRuntimeServiceTest TEST_PREFIX "AiSidecarRuntimeServiceTest."
  PROPERTIES LABELS "ci_core_flow")
alcedo_register_test_target(AiSidecarRuntimeServiceTest app ci_core)

# Frontend 1b: multi-profile provider cards, JSON persistence, and generated sidecar configs.
add_executable(AiProviderProfileTest ${ALCEDO_TEST_ROOT}/app/ai_provider_profile_test.cpp)
target_include_directories(AiProviderProfileTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(AiProviderProfileTest AiProviderProfile AiCredentialStore GTest::gtest_main)
gtest_discover_tests(AiProviderProfileTest TEST_PREFIX "AiProviderProfileTest."
  PROPERTIES LABELS "ci_core_flow")
alcedo_register_test_target(AiProviderProfileTest app ci_core)

add_executable(AiCredentialStoreTest ${ALCEDO_TEST_ROOT}/app/ai_credential_store_test.cpp)
target_include_directories(AiCredentialStoreTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(AiCredentialStoreTest AiCredentialStore GTest::gtest_main)
gtest_discover_tests(AiCredentialStoreTest TEST_PREFIX "AiCredentialStoreTest."
  PROPERTIES LABELS "ci_core_flow")
alcedo_register_test_target(AiCredentialStoreTest app ci_core)

add_executable(SemanticGrpcDependencySmokeTest ${ALCEDO_TEST_ROOT}/app/semantic_grpc_dependency_smoke_test.cpp)
target_include_directories(SemanticGrpcDependencySmokeTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(SemanticGrpcDependencySmokeTest
  puerhlab::grpc++
  puerhlab::protobuf
  GTest::gtest_main)
gtest_discover_tests(SemanticGrpcDependencySmokeTest TEST_PREFIX "SemanticGrpcDependencySmokeTest."
  PROPERTIES LABELS "ci_core_flow")
alcedo_register_test_target(SemanticGrpcDependencySmokeTest app ci_core)

add_executable(SemanticGenerationServiceTest ${ALCEDO_TEST_ROOT}/app/semantic_generation_service_test.cpp)
target_include_directories(SemanticGenerationServiceTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(SemanticGenerationServiceTest
  SemanticGenerationService
  ProjectService
  ImportService
  DuckDB
  GTest::gtest_main)
gtest_discover_tests(SemanticGenerationServiceTest TEST_PREFIX "SemanticGenerationServiceTest."
  PROPERTIES LABELS "ci_raw_flow")
alcedo_register_test_target(SemanticGenerationServiceTest app ci_raw)

# Phase 5d: encoded-rendition encoder + host image-analysis service tests.
add_executable(ImageAnalysisEncoderTest ${ALCEDO_TEST_ROOT}/app/image_analysis_encoder_test.cpp)
target_include_directories(ImageAnalysisEncoderTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(ImageAnalysisEncoderTest ImageAnalysisEncoder ImageBuffer
  GTest::gtest_main)
gtest_discover_tests(ImageAnalysisEncoderTest TEST_PREFIX "ImageAnalysisEncoderTest.")
alcedo_register_test_target(ImageAnalysisEncoderTest app)

add_executable(ImageAnalysisServiceTest ${ALCEDO_TEST_ROOT}/app/image_analysis_service_test.cpp)
target_include_directories(ImageAnalysisServiceTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_compile_definitions(ImageAnalysisServiceTest PRIVATE
  ALCEDO_REPO_ROOT="${CMAKE_SOURCE_DIR}")
target_link_libraries(ImageAnalysisServiceTest ImageAnalysisService
  GTest::gtest_main)
gtest_discover_tests(ImageAnalysisServiceTest TEST_PREFIX "ImageAnalysisServiceTest."
  PROPERTIES LABELS "ci_raw_flow")
alcedo_register_test_target(ImageAnalysisServiceTest app ci_raw)

# Phase 6d: album image-analysis job controller. Standalone QObject module
# (IImageAnalysisEnvironment seam + AiProviderProfileController) driven by fakes.
add_executable(ImageAnalysisControllerTest ${ALCEDO_TEST_ROOT}/app/image_analysis_controller_test.cpp)
target_include_directories(ImageAnalysisControllerTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(ImageAnalysisControllerTest ImageAnalysisController AiProviderProfile
  AiCredentialStore GTest::gtest_main)
gtest_discover_tests(ImageAnalysisControllerTest TEST_PREFIX "ImageAnalysisControllerTest."
  PROPERTIES LABELS "ci_core_flow")
alcedo_register_test_target(ImageAnalysisControllerTest app ci_core)

# Env-gated live smoke: opens a real packed .alcd, materializes one k1024 thumbnail, starts the
# real Rust sidecar (no CLIP model; describe uses the HTTP provider path), and asks a live LLM to
# describe the image through ImageAnalysisService. Skipped unless ALCEDO_IA_LIVE_RUNTIME_PATH,
# ALCEDO_TEST_PACKED_PROJECT_PATH, and ALCEDO_IA_LIVE_ENV_TEST_PATH are set. ProjectService
# transitively provides ProjectPackageService (unpack), AlbumBrowseService (enumerate), and
# AiSidecarRuntimeService (sidecar); ThumbnailService provides the k1024 materialization.
add_executable(ImageAnalysisLiveSmokeTest ${ALCEDO_TEST_ROOT}/app/image_analysis_live_smoke_test.cpp)
target_include_directories(ImageAnalysisLiveSmokeTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(ImageAnalysisLiveSmokeTest ImageAnalysisService ProjectService
  GTest::gtest_main)
gtest_discover_tests(ImageAnalysisLiveSmokeTest TEST_PREFIX "ImageAnalysisLiveSmokeTest."
  PROPERTIES LABELS "ci_raw_flow")
alcedo_register_test_target(ImageAnalysisLiveSmokeTest app ci_raw)

add_executable(ThumbnailServiceTest ${ALCEDO_TEST_ROOT}/app/thumbnail_service_test.cpp)
target_include_directories(ThumbnailServiceTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(ThumbnailServiceTest ThumbnailService ProjectService ImportService GTest::gtest_main)

alcedo_register_test_target(ThumbnailServiceTest app)

add_executable(ThumbnailDiskCacheServiceTest ${ALCEDO_TEST_ROOT}/app/thumbnail_disk_cache_service_test.cpp)
target_include_directories(ThumbnailDiskCacheServiceTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(ThumbnailDiskCacheServiceTest ThumbnailDiskCacheService GTest::gtest_main)
gtest_discover_tests(ThumbnailDiskCacheServiceTest TEST_PREFIX "ThumbnailDiskCacheServiceTest.")
alcedo_register_test_target(ThumbnailDiskCacheServiceTest app)

add_executable(ExportServiceTest ${ALCEDO_TEST_ROOT}/app/export_service_test.cpp)
target_include_directories(ExportServiceTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(ExportServiceTest ExportService ProjectService ImportService GTest::gtest_main ImageWriter ${ALCEDO_TEST_OPENCV_MIN_DEPS})
gtest_discover_tests(ExportServiceTest TEST_PREFIX "ExportServiceTest.")

alcedo_register_test_target(ExportServiceTest app)

add_executable(HsResearchExportTool ${ALCEDO_TEST_ROOT}/app/hs_research_export_tool.cpp)
target_include_directories(HsResearchExportTool PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(HsResearchExportTool PRIVATE ExportService ProjectService ImportService ImageWriter ${ALCEDO_TEST_OPENCV_MIN_DEPS})
alcedo_register_test_target(HsResearchExportTool app)

add_executable(ThumbnailAlbumQtDemo ${ALCEDO_TEST_ROOT}/app/thumbnail_album_qt_demo.cpp)
target_include_directories(ThumbnailAlbumQtDemo PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(ThumbnailAlbumQtDemo PRIVATE ThumbnailService ProjectService ImportService Qt6::Widgets SleeveFilterService)
target_compile_definitions(ThumbnailAlbumQtDemo PRIVATE ALCEDO_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

set(ALCEDO_DEMO_FONT_SRC "${ALCEDO_SRC_DIR}/config/fonts/main_IBM.ttf")
add_custom_command(TARGET ThumbnailAlbumQtDemo POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:ThumbnailAlbumQtDemo>/fonts"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${ALCEDO_DEMO_FONT_SRC}" "$<TARGET_FILE_DIR:ThumbnailAlbumQtDemo>/fonts/main_IBM.ttf"
  VERBATIM
)
alcedo_register_test_target(ThumbnailAlbumQtDemo app)
