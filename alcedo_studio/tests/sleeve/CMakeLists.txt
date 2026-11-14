# Sleeve / storage test target ownership.
include("${ALCEDO_TEST_ROOT}/cmake/AlcedoTestRegistration.cmake")

add_executable(SleeveFSTest ${ALCEDO_TEST_ROOT}/sleeve/sleeve_fs_test.cpp)
target_include_directories(SleeveFSTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(SleeveFSTest PRIVATE GTest::gtest_main SleeveFS StrConv)
gtest_discover_tests(SleeveFSTest TEST_PREFIX "SleeveFSTest.")
alcedo_register_test_target(SleeveFSTest storage)

add_executable(SleeveFolderRestoreTest ${ALCEDO_TEST_ROOT}/sleeve/sleeve_folder_restore_test.cpp)
target_include_directories(SleeveFolderRestoreTest PRIVATE ${ALCEDO_INCLUDE_DIR})
target_link_libraries(SleeveFolderRestoreTest PRIVATE GTest::gtest_main SleeveFS)
gtest_discover_tests(SleeveFolderRestoreTest TEST_PREFIX "SleeveFolderRestoreTest."
  PROPERTIES LABELS "ci_core_flow")
alcedo_register_test_target(SleeveFolderRestoreTest storage ci_core)

add_executable(DuckormExprTest ${ALCEDO_TEST_ROOT}/storage/duckorm_expr_test.cpp)
target_include_directories(DuckormExprTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(DuckormExprTest PRIVATE GTest::gtest_main DuckORM)
gtest_discover_tests(DuckormExprTest TEST_PREFIX "DuckormExprTest."
  PROPERTIES LABELS "ci_core_flow")
alcedo_register_test_target(DuckormExprTest storage ci_core)

add_executable(DuckormStatementTest ${ALCEDO_TEST_ROOT}/storage/duckorm_statement_test.cpp)
target_include_directories(DuckormStatementTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(DuckormStatementTest PRIVATE GTest::gtest_main DuckORM)
gtest_discover_tests(DuckormStatementTest TEST_PREFIX "DuckormStatementTest."
  PROPERTIES LABELS "ci_core_flow")
alcedo_register_test_target(DuckormStatementTest storage ci_core)

# Phase 3: merged CRTP Mapper base — ToParams/FromParams and SqlFragment WHERE round-trip.
add_executable(MapperCrtpRoundtripTest ${ALCEDO_TEST_ROOT}/storage/mapper_crtp_roundtrip_test.cpp)
target_include_directories(MapperCrtpRoundtripTest PUBLIC ${ALCEDO_INCLUDE_DIR} ${ALCEDO_TEST_DIR})
target_link_libraries(MapperCrtpRoundtripTest PRIVATE GTest::gtest_main Storage SleeveMapper Image StrConv)
gtest_discover_tests(MapperCrtpRoundtripTest TEST_PREFIX "MapperCrtpRoundtripTest."
  PROPERTIES LABELS "ci_core_flow")
alcedo_register_test_target(MapperCrtpRoundtripTest storage ci_core)

# Compile-path unit checks for FilterNode -> SqlFragment (no folder fixtures).
add_executable(SleeveFilterCompileTest ${ALCEDO_TEST_ROOT}/sleeve/sleeve_filter_compile_test.cpp)
target_include_directories(SleeveFilterCompileTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(SleeveFilterCompileTest PRIVATE GTest::gtest_main StorageQuery StrConv)
gtest_discover_tests(SleeveFilterCompileTest TEST_PREFIX "SleeveFilterCompileTest."
  PROPERTIES LABELS "ci_core_flow")
alcedo_register_test_target(SleeveFilterCompileTest storage ci_core)

# Compile-path unit checks for the sleeve filter factories (stats-bar bucket
# predicates, semantic-label EXISTS, merge/compile entry helpers).
add_executable(SleeveFilterFactoryTest ${ALCEDO_TEST_ROOT}/sleeve/sleeve_filter_factory_test.cpp)
target_include_directories(SleeveFilterFactoryTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(SleeveFilterFactoryTest PRIVATE GTest::gtest_main StorageQuery StrConv)
gtest_discover_tests(SleeveFilterFactoryTest TEST_PREFIX "SleeveFilterFactoryTest."
  PROPERTIES LABELS "ci_core_flow")
alcedo_register_test_target(SleeveFilterFactoryTest storage ci_core)

# Storage S4 (#789): the bucket predicate of each stats dimension selects its bucket count.
add_executable(StatsDimensionTest ${ALCEDO_TEST_ROOT}/storage/stats_dimension_test.cpp)
target_include_directories(StatsDimensionTest PUBLIC ${ALCEDO_INCLUDE_DIR} ${ALCEDO_TEST_DIR} ${ALCEDO_TEST_ROOT}/app)
target_link_libraries(StatsDimensionTest PRIVATE GTest::gtest_main ProjectService SleeveFilterService)
gtest_discover_tests(StatsDimensionTest TEST_PREFIX "StatsDimensionTest."
  PROPERTIES LABELS "ci_core_flow")
alcedo_copy_duckdb_extensions(StatsDimensionTest)
alcedo_register_test_target(StatsDimensionTest storage ci_core)

# Storage S5 (#790): the library search predicate is built in storage from the parsed request;
# user text is a bound value.
add_executable(LibrarySearchPredicatesTest
  ${ALCEDO_TEST_ROOT}/storage/library_search_predicates_test.cpp)
target_include_directories(LibrarySearchPredicatesTest PUBLIC ${ALCEDO_INCLUDE_DIR} ${ALCEDO_TEST_DIR}
  ${ALCEDO_TEST_ROOT}/app)
target_link_libraries(LibrarySearchPredicatesTest PRIVATE GTest::gtest_main ProjectService
  SleeveFilterService)
gtest_discover_tests(LibrarySearchPredicatesTest TEST_PREFIX "LibrarySearchPredicatesTest."
  PROPERTIES LABELS "ci_core_flow")
alcedo_copy_duckdb_extensions(LibrarySearchPredicatesTest)
alcedo_register_test_target(LibrarySearchPredicatesTest storage ci_core)

add_executable(DuckDbExtensionTest ${ALCEDO_TEST_ROOT}/storage/duckdb_extension_test.cpp)
target_include_directories(DuckDbExtensionTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(DuckDbExtensionTest PRIVATE GTest::gtest_main Storage)
gtest_discover_tests(DuckDbExtensionTest TEST_PREFIX "DuckDbExtensionTest.")
alcedo_copy_duckdb_extensions(DuckDbExtensionTest)
alcedo_register_test_target(DuckDbExtensionTest storage)

# AI Segmentation Mask R8 assets in the project database (put once per key, zlib, verified read).
add_executable(MaskAssetStoreTest ${ALCEDO_TEST_ROOT}/storage/mask_asset_store_test.cpp)
target_include_directories(MaskAssetStoreTest PUBLIC ${ALCEDO_INCLUDE_DIR} ${ALCEDO_TEST_DIR})
target_link_libraries(MaskAssetStoreTest PRIVATE GTest::gtest_main Storage)
gtest_discover_tests(MaskAssetStoreTest TEST_PREFIX "MaskAssetStoreTest."
  PROPERTIES LABELS "storage;ai_mask")
alcedo_register_test_target(MaskAssetStoreTest storage)

# Storage::OpenSession: the database lock for the session lifetime and the image root write.
add_executable(StorageSessionTest ${ALCEDO_TEST_ROOT}/storage/storage_session_test.cpp)
target_include_directories(StorageSessionTest PUBLIC ${ALCEDO_INCLUDE_DIR} ${ALCEDO_TEST_DIR})
target_link_libraries(StorageSessionTest PRIVATE GTest::gtest_main Storage)
gtest_discover_tests(StorageSessionTest TEST_PREFIX "StorageSessionTest."
  PROPERTIES LABELS "storage")
alcedo_register_test_target(StorageSessionTest storage)

# Storage::Maintenance: the diagnostics summary, the package snapshot, and the HNSW index drop.
add_executable(StorageMaintenanceTest ${ALCEDO_TEST_ROOT}/storage/storage_maintenance_test.cpp)
target_include_directories(StorageMaintenanceTest PUBLIC ${ALCEDO_INCLUDE_DIR} ${ALCEDO_TEST_DIR})
target_link_libraries(StorageMaintenanceTest PRIVATE GTest::gtest_main Storage)
gtest_discover_tests(StorageMaintenanceTest TEST_PREFIX "StorageMaintenanceTest."
  PROPERTIES LABELS "storage")
alcedo_copy_duckdb_extensions(StorageMaintenanceTest)
alcedo_register_test_target(StorageMaintenanceTest storage)

# AI annotation and semantic model/embedding stores against a real project database.
add_executable(AiStorageControllerTest ${ALCEDO_TEST_ROOT}/storage/ai_storage_controller_test.cpp)
target_include_directories(AiStorageControllerTest PUBLIC ${ALCEDO_INCLUDE_DIR} ${ALCEDO_TEST_DIR})
target_link_libraries(AiStorageControllerTest PRIVATE GTest::gtest_main ProjectService)
gtest_discover_tests(AiStorageControllerTest TEST_PREFIX "AiStorageControllerTest.")
alcedo_copy_duckdb_extensions(AiStorageControllerTest)
alcedo_register_test_target(AiStorageControllerTest storage)

add_executable(SemanticStorageControllerTest
  ${ALCEDO_TEST_ROOT}/storage/semantic_storage_controller_test.cpp)
target_include_directories(SemanticStorageControllerTest PUBLIC ${ALCEDO_INCLUDE_DIR} ${ALCEDO_TEST_DIR})
target_link_libraries(SemanticStorageControllerTest PRIVATE GTest::gtest_main ProjectService)
gtest_discover_tests(SemanticStorageControllerTest TEST_PREFIX "SemanticStorageControllerTest.")
alcedo_copy_duckdb_extensions(SemanticStorageControllerTest)
alcedo_register_test_target(SemanticStorageControllerTest storage)
