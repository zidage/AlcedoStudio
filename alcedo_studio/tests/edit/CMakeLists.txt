# Mini-Git history and save-capture test target ownership.
set(ALCEDO_TEST_EDIT_HISTORY_ROOT "${ALCEDO_TEST_ROOT}/edit/history")
include("${ALCEDO_TEST_ROOT}/cmake/AlcedoTestRegistration.cmake")

# Phase 5H: recovery REDO, atomic DuckDB materialization, compaction, fault injection.
add_executable(EditorHistoryMaterializerTest ${ALCEDO_TEST_ROOT}/edit/history/editor_history_materializer_test.cpp)
target_include_directories(EditorHistoryMaterializerTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(EditorHistoryMaterializerTest
  EditorHistoryMaterializer
  ProjectService
  EditHistory
  GTest::gtest_main)
gtest_discover_tests(EditorHistoryMaterializerTest TEST_PREFIX "EditorHistoryMaterializerTest."
  PROPERTIES LABELS "ci_core_flow")
alcedo_register_test_target(EditorHistoryMaterializerTest edit)

# WAL alignment / unique-history replay helpers (replaced production JournalFold).
add_executable(EditorMiniGitJournalFoldTest ${ALCEDO_TEST_ROOT}/edit/history/editor_mini_git_journal_fold_test.cpp)
target_include_directories(EditorMiniGitJournalFoldTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(EditorMiniGitJournalFoldTest
  EditHistory
  GTest::gtest_main)
gtest_discover_tests(EditorMiniGitJournalFoldTest TEST_PREFIX "EditorMiniGitJournalFoldTest."
  PROPERTIES LABELS "ci_core_flow;edit;history")
alcedo_register_test_target(EditorMiniGitJournalFoldTest edit)

# Phase 6C-5: commit writer DuckDB transaction tests with failure injection.
add_executable(EditorMiniGitCommitWriterTest ${ALCEDO_TEST_ROOT}/edit/history/editor_mini_git_commit_writer_test.cpp)
target_include_directories(EditorMiniGitCommitWriterTest
  PUBLIC ${ALCEDO_INCLUDE_DIR} ${ALCEDO_TEST_DIR})
target_link_libraries(EditorMiniGitCommitWriterTest
  EditorSessionTestSupport
  EditorMiniGitCommitWriter
  EditorMiniGitMaterializer
  EditorSaveCheckpointCoordinator
  ProjectService
  EditHistory
  Storage
  GTest::gtest_main)
gtest_discover_tests(EditorMiniGitCommitWriterTest TEST_PREFIX "EditorMiniGitCommitWriterTest."
  PROPERTIES LABELS "ci_core_flow;edit;history;editor_history")
alcedo_register_test_target(EditorMiniGitCommitWriterTest edit)

# Journal recovery: fully-covered clear, contiguous suffix leave, broken isolate.
add_executable(EditorMiniGitJournalRecoveryTest ${ALCEDO_TEST_ROOT}/edit/history/editor_mini_git_journal_recovery_test.cpp)
target_include_directories(EditorMiniGitJournalRecoveryTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(EditorMiniGitJournalRecoveryTest
  EditorMiniGitJournalRecovery
  EditorMiniGitCommitWriter
  ProjectService
  EditHistory
  Storage
  GTest::gtest_main)
gtest_discover_tests(EditorMiniGitJournalRecoveryTest TEST_PREFIX "EditorMiniGitJournalRecoveryTest."
  PROPERTIES LABELS "ci_core_flow;edit;history")
alcedo_register_test_target(EditorMiniGitJournalRecoveryTest edit)

# Exact journal-range capture: sequence bounds, post-capture append survival, mismatch rejection.
add_executable(EditorSaveCheckpointCaptureTest
  ${ALCEDO_TEST_ROOT}/edit/history/editor_save_checkpoint_capture_test.cpp)
target_include_directories(EditorSaveCheckpointCaptureTest
  PUBLIC ${ALCEDO_INCLUDE_DIR} ${ALCEDO_TEST_DIR})
target_link_libraries(EditorSaveCheckpointCaptureTest
  EditorSessionTestSupport
  EditorMiniGitMaterializer
  EditorSaveCheckpointCoordinator
  ProjectService
  EditHistory
  Storage
  GTest::gtest_main)
gtest_discover_tests(EditorSaveCheckpointCaptureTest TEST_PREFIX "EditorSaveCheckpointCaptureTest."
  PROPERTIES LABELS "ci_core_flow;edit;history;editor_history")
alcedo_register_test_target(EditorSaveCheckpointCaptureTest edit)

# Phase 6C-5: mini-Git save checkpoint materialization facade tests.
add_executable(EditorMiniGitMaterializerTest ${ALCEDO_TEST_ROOT}/edit/history/editor_mini_git_materializer_test.cpp)
target_include_directories(EditorMiniGitMaterializerTest
  PUBLIC ${ALCEDO_INCLUDE_DIR} ${ALCEDO_TEST_DIR})
target_link_libraries(EditorMiniGitMaterializerTest
  EditorSessionTestSupport
  EditorMiniGitMaterializer
  EditorSaveCheckpointCoordinator
  ProjectService
  EditHistory
  Storage
  GTest::gtest_main)
gtest_discover_tests(EditorMiniGitMaterializerTest TEST_PREFIX "EditorMiniGitMaterializerTest."
  PROPERTIES LABELS "ci_core_flow;edit;history;editor_history")
alcedo_register_test_target(EditorMiniGitMaterializerTest edit)

# Tone mapping ownership: op assignment, op-array roundtrip, JSON.
add_executable(ToneMappingOwnershipTest ${ALCEDO_TEST_ROOT}/edit/operators/tone/tone_mapping_ownership_test.cpp)
target_compile_definitions(ToneMappingOwnershipTest
    PRIVATE ALCEDO_SOURCE_ROOT="${CMAKE_SOURCE_DIR}/alcedo_studio/src")
target_link_libraries(ToneMappingOwnershipTest PRIVATE GTest::gtest_main)
gtest_discover_tests(ToneMappingOwnershipTest TEST_PREFIX "ToneMappingOwnershipTest.")
alcedo_register_test_target(ToneMappingOwnershipTest edit)

# Local tone mapping contract: default construction, setters/getters.
add_executable(LocalToneMappingContractTest ${ALCEDO_TEST_ROOT}/edit/operators/tone/local_tone_mapping_contract_test.cpp)
target_compile_definitions(LocalToneMappingContractTest
    PRIVATE ALCEDO_SOURCE_ROOT="${CMAKE_SOURCE_DIR}/alcedo_studio/src")
target_include_directories(LocalToneMappingContractTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(LocalToneMappingContractTest PRIVATE GTest::gtest_main)
gtest_discover_tests(LocalToneMappingContractTest TEST_PREFIX "LocalToneMappingContractTest.")
alcedo_register_test_target(LocalToneMappingContractTest edit)

# Tone mapping facade: parameter interfaces.
add_executable(ToneMappingFacadeTest ${ALCEDO_TEST_ROOT}/edit/operators/tone/tone_mapping_facade_test.cpp)
target_include_directories(ToneMappingFacadeTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(ToneMappingFacadeTest PRIVATE GTest::gtest_main Operators)
gtest_discover_tests(ToneMappingFacadeTest TEST_PREFIX "ToneMappingFacadeTest.")
alcedo_register_test_target(ToneMappingFacadeTest edit)

# ODT (Output Device Transform) operator test.
add_executable(ODTOpTest ${ALCEDO_TEST_ROOT}/edit/operators/cst/odt_op_test.cpp)
target_include_directories(ODTOpTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(ODTOpTest PRIVATE GTest::gtest_main Operators)
gtest_discover_tests(ODTOpTest TEST_PREFIX "ODTOpTest.")
alcedo_register_test_target(ODTOpTest edit)

# Film grain operator test.
add_executable(FilmGrainOpTest ${ALCEDO_TEST_ROOT}/edit/operators/cst/film_grain_op_test.cpp)
target_include_directories(FilmGrainOpTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(FilmGrainOpTest PRIVATE GTest::gtest_main Operators)
gtest_discover_tests(FilmGrainOpTest TEST_PREFIX "FilmGrainOpTest.")
alcedo_register_test_target(FilmGrainOpTest edit)

# Halation operator test.
add_executable(HalationOpTest ${ALCEDO_TEST_ROOT}/edit/operators/cst/halation_op_test.cpp)
target_include_directories(HalationOpTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(HalationOpTest PRIVATE GTest::gtest_main Operators)
gtest_discover_tests(HalationOpTest TEST_PREFIX "HalationOpTest.")
alcedo_register_test_target(HalationOpTest edit)

# Crop and rotate operator test.
add_executable(CropRotateOpTest ${ALCEDO_TEST_ROOT}/edit/operators/geometry/crop_rotate_op_test.cpp)
target_include_directories(CropRotateOpTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(CropRotateOpTest PRIVATE GTest::gtest_main Operators ${ALCEDO_TEST_OPENCV_MIN_DEPS})
gtest_discover_tests(CropRotateOpTest TEST_PREFIX "CropRotateOpTest."
  PROPERTIES LABELS "ci_core_flow")
alcedo_register_test_target(CropRotateOpTest edit)

# RAW decode durable params: inherent RawColorContext round-trip, omit decode_res.
add_executable(RawDecodeOpParamsTest
  ${ALCEDO_TEST_ROOT}/edit/operators/raw/raw_decode_op_params_test.cpp)
target_include_directories(RawDecodeOpParamsTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(RawDecodeOpParamsTest PRIVATE GTest::gtest_main Operators Image)
gtest_discover_tests(RawDecodeOpParamsTest TEST_PREFIX "RawDecodeOpParamsTest."
  PROPERTIES LABELS "ci_core_flow;edit;operators")
alcedo_register_test_target(RawDecodeOpParamsTest edit)

# ColorTemp durable params: as_shot_* vs custom_* keys and legacy aliases.
add_executable(ColorTempOpParamsTest
  ${ALCEDO_TEST_ROOT}/edit/operators/basic/color_temp_op_params_test.cpp)
target_include_directories(ColorTempOpParamsTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(ColorTempOpParamsTest PRIVATE GTest::gtest_main Operators)
# Skip post-build runtime DLL staging: this is a header/params unit test with no GPU path.
set_target_properties(ColorTempOpParamsTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY
  "${CMAKE_CURRENT_BINARY_DIR}/ColorTempOpParamsTest_runtime")
gtest_discover_tests(ColorTempOpParamsTest TEST_PREFIX "ColorTempOpParamsTest."
  PROPERTIES LABELS "ci_core_flow;edit;operators")
alcedo_register_test_target(ColorTempOpParamsTest edit)

# Pipeline frame sink: CPU frame delivery contract.
add_executable(PipelineFrameSinkTest ${ALCEDO_TEST_ROOT}/edit/pipeline/pipeline_frame_sink_test.cpp)
target_include_directories(PipelineFrameSinkTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(PipelineFrameSinkTest PRIVATE GTest::gtest_main Operators EditPipeline
                                                    PipelineScheduler ${ALCEDO_TEST_OPENCV_MIN_DEPS})
gtest_discover_tests(PipelineFrameSinkTest TEST_PREFIX "PipelineFrameSinkTest.")
alcedo_register_test_target(PipelineFrameSinkTest edit)

add_executable(PipelineSchedulerRequestIdTest
    ${ALCEDO_TEST_ROOT}/renderer/pipeline_scheduler_request_id_test.cpp)
target_include_directories(PipelineSchedulerRequestIdTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(PipelineSchedulerRequestIdTest PRIVATE GTest::gtest_main Operators EditPipeline
                                                    PipelineScheduler EditorRhiViewport
                                                    ${ALCEDO_TEST_OPENCV_MIN_DEPS})
gtest_discover_tests(PipelineSchedulerRequestIdTest TEST_PREFIX "PipelineSchedulerRequestIdTest.")
alcedo_register_test_target(PipelineSchedulerRequestIdTest edit)

# Version hash: deterministic parameter hashing.
add_executable(VersionHashTest ${ALCEDO_TEST_ROOT}/edit/history/version_hash_test.cpp)
target_include_directories(VersionHashTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(VersionHashTest EditHistory GTest::gtest_main)
gtest_discover_tests(VersionHashTest TEST_PREFIX "VersionHashTest."
  PROPERTIES LABELS "ci_core_flow")
alcedo_register_test_target(VersionHashTest edit)

# Commit graph: topology, LCA, serialization.
add_executable(CommitGraphTest ${ALCEDO_TEST_ROOT}/edit/history/commit_graph_test.cpp)
target_include_directories(CommitGraphTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(CommitGraphTest ProjectService EditHistory Storage GTest::gtest_main)
gtest_discover_tests(CommitGraphTest TEST_PREFIX "CommitGraphTest."
  PROPERTIES LABELS "ci_core_flow;edit;history;commit_graph")
alcedo_register_test_target(CommitGraphTest edit)

# Redo-only journal record format, RewriteTimeline, independent timeline simulator.
add_executable(EditorTransactionJournalTest ${ALCEDO_TEST_ROOT}/edit/history/editor_transaction_journal_test.cpp)
target_include_directories(EditorTransactionJournalTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(EditorTransactionJournalTest EditHistory GTest::gtest_main)
gtest_discover_tests(EditorTransactionJournalTest TEST_PREFIX "EditorTransactionJournalTest."
  PROPERTIES LABELS "ci_core_flow")
alcedo_register_test_target(EditorTransactionJournalTest edit)

# Image-scoped journal batching, flush durability, and committed replay.
add_executable(EditorJournalWriterTest ${ALCEDO_TEST_ROOT}/edit/history/editor_journal_writer_test.cpp)
target_include_directories(EditorJournalWriterTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(EditorJournalWriterTest EditHistory GTest::gtest_main)
gtest_discover_tests(EditorJournalWriterTest TEST_PREFIX "EditorJournalWriterTest."
  PROPERTIES LABELS "ci_core_flow")
alcedo_register_test_target(EditorJournalWriterTest edit)

# Deterministic seeded WAL restart/fault framework. Deliberately stays
# test-side and dependency-free; scheduled CI can increase the step count.
add_executable(EditorJournalFuzzFrameworkTest
  ${ALCEDO_TEST_ROOT}/edit/history/editor_journal_fuzz_framework.cpp
  ${ALCEDO_TEST_ROOT}/edit/history/editor_journal_fuzz_test.cpp)
target_include_directories(EditorJournalFuzzFrameworkTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(EditorJournalFuzzFrameworkTest EditHistory GTest::gtest_main)
target_compile_definitions(EditorJournalFuzzFrameworkTest PRIVATE
  EDITOR_JOURNAL_FUZZ_CORPUS_PATH="${ALCEDO_TEST_ROOT}/edit/history/editor_journal_fuzz_seeds.txt")
gtest_discover_tests(EditorJournalFuzzFrameworkTest TEST_PREFIX "EditorJournalFuzzFrameworkTest."
  PROPERTIES LABELS "ci_core_flow")
alcedo_register_test_target(EditorJournalFuzzFrameworkTest edit)

# GPU DAG Phase G1: Model, DTO, PipelineGraph, format v2 JSON, legacy import.
add_executable(GpuDagModelGraphTest
  ${ALCEDO_TEST_ROOT}/edit/graph/default_pipeline_test.cpp
  ${ALCEDO_TEST_ROOT}/edit/graph/graph_validation_test.cpp
  ${ALCEDO_TEST_ROOT}/edit/graph/dirty_patch_test.cpp
  ${ALCEDO_TEST_ROOT}/edit/graph/json_roundtrip_test.cpp
  ${ALCEDO_TEST_ROOT}/edit/graph/legacy_import_test.cpp
  ${ALCEDO_TEST_ROOT}/edit/graph/header_hygiene_test.cpp)
target_include_directories(GpuDagModelGraphTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(GpuDagModelGraphTest PRIVATE GTest::gtest_main EditGraph)
target_compile_definitions(GpuDagModelGraphTest PRIVATE
  ALCEDO_MODEL_HEADER_ROOT="${ALCEDO_INCLUDE_DIR}/edit/operators/models"
  ALCEDO_GRAPH_HEADER_ROOT="${ALCEDO_INCLUDE_DIR}/edit/graph")
gtest_discover_tests(GpuDagModelGraphTest TEST_PREFIX "GpuDagModelGraphTest."
  PROPERTIES LABELS "ci_core_flow;edit;graph")
alcedo_register_test_target(GpuDagModelGraphTest edit)

# GPU DAG Phase G3: RenderGeometryResolver (CPU).
add_executable(GpuDagGeometryTest
  ${ALCEDO_TEST_ROOT}/edit/geometry/render_geometry_resolver_test.cpp
  ${ALCEDO_TEST_ROOT}/edit/geometry/sampling_and_document_test.cpp)
target_include_directories(GpuDagGeometryTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(GpuDagGeometryTest PRIVATE GTest::gtest_main EditGeometry EditGraph)
target_compile_definitions(GpuDagGeometryTest PRIVATE
  ALCEDO_MODEL_HEADER_ROOT="${ALCEDO_INCLUDE_DIR}/edit/operators/models"
  ALCEDO_GEOMETRY_HEADER_ROOT="${ALCEDO_INCLUDE_DIR}/edit/geometry")
gtest_discover_tests(GpuDagGeometryTest TEST_PREFIX "GpuDagGeometryTest."
  PROPERTIES LABELS "ci_core_flow;edit;geometry")
alcedo_register_test_target(GpuDagGeometryTest edit)

# GPU DAG Phase G6: persistent R8 repository and GPU-free host cache.
add_executable(GpuDagMaskStoreTest
  ${ALCEDO_TEST_ROOT}/edit/mask/mask_store_test.cpp)
target_include_directories(GpuDagMaskStoreTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(GpuDagMaskStoreTest PRIVATE GTest::gtest_main EditMaskStore)
gtest_discover_tests(GpuDagMaskStoreTest TEST_PREFIX "GpuDagMaskStoreTest."
  PROPERTIES LABELS "ci_core_flow;edit;mask")
alcedo_register_test_target(GpuDagMaskStoreTest edit)

# GPU DAG Phase G3: CUDA GeometryResamplePass.
if(ALCEDO_CUDA_ENABLED)
  add_executable(GpuDagCudaGeometryTest
    ${ALCEDO_TEST_ROOT}/edit/geometry/cuda_geometry_resample_test.cpp)
  target_include_directories(GpuDagCudaGeometryTest PUBLIC ${ALCEDO_INCLUDE_DIR})
  target_link_libraries(GpuDagCudaGeometryTest PRIVATE GTest::gtest_main EditRuntimeCuda)
  gtest_discover_tests(GpuDagCudaGeometryTest TEST_PREFIX "GpuDagCudaGeometryTest."
    PROPERTIES LABELS "gpu;edit;geometry")
  alcedo_register_test_target(GpuDagCudaGeometryTest gpu)
endif()

# GPU DAG Phase G4: CPU RawInputLoader / GraphCompiler (no CUDA required).
add_executable(GpuDagRawInputTest
  ${ALCEDO_TEST_ROOT}/edit/input/raw_input_loader_test.cpp
  ${ALCEDO_TEST_ROOT}/edit/input/prepared_source_cache_test.cpp
  ${ALCEDO_TEST_ROOT}/edit/runtime/graph_compiler_test.cpp
  ${ALCEDO_TEST_ROOT}/edit/runtime/static_execution_plan_cache_test.cpp)
target_include_directories(GpuDagRawInputTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(GpuDagRawInputTest PRIVATE GTest::gtest_main EditInput EditRuntime)
target_compile_definitions(GpuDagRawInputTest PRIVATE
  ALCEDO_RUNTIME_HEADER_ROOT="${ALCEDO_INCLUDE_DIR}/edit/runtime"
  ALCEDO_INPUT_HEADER_ROOT="${ALCEDO_INCLUDE_DIR}/edit/input")
gtest_discover_tests(GpuDagRawInputTest TEST_PREFIX "GpuDagRawInputTest."
  PROPERTIES LABELS "ci_core_flow;edit;input")
alcedo_register_test_target(GpuDagRawInputTest edit)

# GPU DAG Phase G4: CUDA Develop Endpoint.
if(ALCEDO_CUDA_ENABLED)
  add_executable(GpuDagCudaDevelopTest
    ${ALCEDO_TEST_ROOT}/edit/runtime/cuda_develop_test.cpp)
  target_include_directories(GpuDagCudaDevelopTest PUBLIC ${ALCEDO_INCLUDE_DIR})
  target_link_libraries(GpuDagCudaDevelopTest PRIVATE GTest::gtest_main EditRuntimeCuda EditInput)
  gtest_discover_tests(GpuDagCudaDevelopTest TEST_PREFIX "GpuDagCudaDevelopTest."
    PROPERTIES LABELS "gpu;edit;runtime")
  alcedo_register_test_target(GpuDagCudaDevelopTest gpu)
endif()
if(ALCEDO_CUDA_ENABLED)
  add_executable(GpuDagCudaPrimaryGradeTest
    ${ALCEDO_TEST_ROOT}/edit/runtime/cuda_primary_grade_test.cpp)
  target_include_directories(GpuDagCudaPrimaryGradeTest PUBLIC ${ALCEDO_INCLUDE_DIR})
  target_link_libraries(GpuDagCudaPrimaryGradeTest PRIVATE GTest::gtest_main EditRuntimeCuda EditInput)
  gtest_discover_tests(GpuDagCudaPrimaryGradeTest TEST_PREFIX "GpuDagCudaPrimaryGradeTest."
    PROPERTIES LABELS "gpu;edit;runtime")
  alcedo_register_test_target(GpuDagCudaPrimaryGradeTest gpu)

  add_executable(GpuDagCudaDrtProductTest
    ${ALCEDO_TEST_ROOT}/edit/runtime/cuda_drt_product_test.cpp
    ${ALCEDO_TEST_ROOT}/edit/runtime/cuda_product_plan_cache_test.cpp)
  target_include_directories(GpuDagCudaDrtProductTest PUBLIC ${ALCEDO_INCLUDE_DIR})
  target_link_libraries(GpuDagCudaDrtProductTest PRIVATE
    GTest::gtest_main EditRuntimeCuda EditInput Image)
  gtest_discover_tests(GpuDagCudaDrtProductTest TEST_PREFIX "GpuDagCudaDrtProductTest."
    PROPERTIES LABELS "gpu;edit;runtime;product")
  alcedo_register_test_target(GpuDagCudaDrtProductTest gpu)

  add_executable(GpuDagCudaMaskTest
    ${ALCEDO_TEST_ROOT}/edit/runtime/cuda_mask_test.cpp)
  target_include_directories(GpuDagCudaMaskTest PUBLIC ${ALCEDO_INCLUDE_DIR})
  target_link_libraries(GpuDagCudaMaskTest PRIVATE GTest::gtest_main EditRuntimeCuda EditInput)
  gtest_discover_tests(GpuDagCudaMaskTest TEST_PREFIX "GpuDagCudaMaskTest."
    PROPERTIES LABELS "gpu;edit;runtime;mask")
  alcedo_register_test_target(GpuDagCudaMaskTest gpu)

  add_executable(GpuDagCudaWorkspaceTest
    ${ALCEDO_TEST_ROOT}/edit/runtime/parameter_arena_test.cpp
    ${ALCEDO_TEST_ROOT}/edit/runtime/transient_and_cache_test.cpp
    ${ALCEDO_TEST_ROOT}/edit/runtime/texture_lease_test.cpp
    ${ALCEDO_TEST_ROOT}/edit/runtime/header_hygiene_test.cpp)
  target_include_directories(GpuDagCudaWorkspaceTest PUBLIC ${ALCEDO_INCLUDE_DIR})
  target_link_libraries(GpuDagCudaWorkspaceTest PRIVATE GTest::gtest_main EditRuntimeCuda)
  target_compile_definitions(GpuDagCudaWorkspaceTest PRIVATE
    ALCEDO_GPU_HEADER_ROOT="${ALCEDO_INCLUDE_DIR}/gpu"
    ALCEDO_RUNTIME_HEADER_ROOT="${ALCEDO_INCLUDE_DIR}/edit/runtime")
  gtest_discover_tests(GpuDagCudaWorkspaceTest TEST_PREFIX "GpuDagCudaWorkspaceTest."
    PROPERTIES LABELS "gpu;edit;runtime")
  alcedo_register_test_target(GpuDagCudaWorkspaceTest gpu)
endif()
