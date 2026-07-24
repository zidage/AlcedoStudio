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

# Phase 6C-5: pure journal fold algorithm tests (no database or file I/O).
add_executable(EditorMiniGitJournalFoldTest ${ALCEDO_TEST_ROOT}/edit/history/editor_mini_git_journal_fold_test.cpp)
target_include_directories(EditorMiniGitJournalFoldTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(EditorMiniGitJournalFoldTest
  EditorMiniGitJournalFold
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
  StorageService
  GTest::gtest_main)
gtest_discover_tests(EditorMiniGitCommitWriterTest TEST_PREFIX "EditorMiniGitCommitWriterTest."
  PROPERTIES LABELS "ci_core_flow;edit;history;editor_history")
alcedo_register_test_target(EditorMiniGitCommitWriterTest edit)

# Phase 6C-5: journal recovery tests (load, detect materialized prefix, truncate).
add_executable(EditorMiniGitJournalRecoveryTest ${ALCEDO_TEST_ROOT}/edit/history/editor_mini_git_journal_recovery_test.cpp)
target_include_directories(EditorMiniGitJournalRecoveryTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(EditorMiniGitJournalRecoveryTest
  EditorMiniGitJournalRecovery
  EditorMiniGitJournalFold
  EditorMiniGitCommitWriter
  ProjectService
  EditHistory
  StorageService
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
  StorageService
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
  StorageService
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

# Pipeline frame sink: CPU frame delivery contract.
add_executable(PipelineFrameSinkTest ${ALCEDO_TEST_ROOT}/edit/pipeline/pipeline_frame_sink_test.cpp)
target_include_directories(PipelineFrameSinkTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(PipelineFrameSinkTest PRIVATE GTest::gtest_main Operators EditPipeline
                                                    PipelineScheduler ${ALCEDO_TEST_OPENCV_MIN_DEPS})
gtest_discover_tests(PipelineFrameSinkTest TEST_PREFIX "PipelineFrameSinkTest.")
alcedo_register_test_target(PipelineFrameSinkTest edit)

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
target_link_libraries(CommitGraphTest ProjectService EditHistory StorageService GTest::gtest_main)
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
