# Sleeve / storage test target ownership.
include("${ALCEDO_TEST_ROOT}/cmake/AlcedoTestRegistration.cmake")

add_executable(SleeveFSTest ${ALCEDO_TEST_ROOT}/sleeve/sleeve_fs_test.cpp)
target_include_directories(SleeveFSTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(SleeveFSTest PRIVATE GTest::gtest_main SleeveFS StrConv)
gtest_discover_tests(SleeveFSTest TEST_PREFIX "SleeveFSTest.")
alcedo_register_test_target(SleeveFSTest storage)

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
target_include_directories(MapperCrtpRoundtripTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(MapperCrtpRoundtripTest PRIVATE GTest::gtest_main Storage SleeveMapper Image StrConv)
gtest_discover_tests(MapperCrtpRoundtripTest TEST_PREFIX "MapperCrtpRoundtripTest."
  PROPERTIES LABELS "ci_core_flow")
alcedo_register_test_target(MapperCrtpRoundtripTest storage ci_core)

# Compile-path unit checks for FilterNode -> SqlFragment (no folder fixtures).
add_executable(SleeveFilterCompileTest ${ALCEDO_TEST_ROOT}/sleeve/sleeve_filter_compile_test.cpp)
target_include_directories(SleeveFilterCompileTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(SleeveFilterCompileTest PRIVATE GTest::gtest_main SleeveFilter StrConv)
gtest_discover_tests(SleeveFilterCompileTest TEST_PREFIX "SleeveFilterCompileTest."
  PROPERTIES LABELS "ci_core_flow")
alcedo_register_test_target(SleeveFilterCompileTest storage ci_core)

# Compile-path unit checks for the sleeve filter factories (stats-bar bucket
# predicates, semantic-label EXISTS, merge/compile entry helpers).
add_executable(SleeveFilterFactoryTest ${ALCEDO_TEST_ROOT}/sleeve/sleeve_filter_factory_test.cpp)
target_include_directories(SleeveFilterFactoryTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(SleeveFilterFactoryTest PRIVATE GTest::gtest_main SleeveFilter StrConv)
gtest_discover_tests(SleeveFilterFactoryTest TEST_PREFIX "SleeveFilterFactoryTest."
  PROPERTIES LABELS "ci_core_flow")
alcedo_register_test_target(SleeveFilterFactoryTest storage ci_core)

add_executable(DuckDbExtensionTest ${ALCEDO_TEST_ROOT}/storage/duckdb_extension_test.cpp)
target_include_directories(DuckDbExtensionTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(DuckDbExtensionTest PRIVATE GTest::gtest_main Storage)
gtest_discover_tests(DuckDbExtensionTest TEST_PREFIX "DuckDbExtensionTest.")
alcedo_copy_duckdb_extensions(DuckDbExtensionTest)
alcedo_register_test_target(DuckDbExtensionTest storage)

# AI annotation and semantic model/embedding stores against a real project database.
add_executable(AiStorageControllerTest ${ALCEDO_TEST_ROOT}/storage/ai_storage_controller_test.cpp)
target_include_directories(AiStorageControllerTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(AiStorageControllerTest PRIVATE GTest::gtest_main ProjectService)
gtest_discover_tests(AiStorageControllerTest TEST_PREFIX "AiStorageControllerTest.")
alcedo_copy_duckdb_extensions(AiStorageControllerTest)
alcedo_register_test_target(AiStorageControllerTest storage)

add_executable(SemanticStorageControllerTest
  ${ALCEDO_TEST_ROOT}/storage/semantic_storage_controller_test.cpp)
target_include_directories(SemanticStorageControllerTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(SemanticStorageControllerTest PRIVATE GTest::gtest_main ProjectService)
gtest_discover_tests(SemanticStorageControllerTest TEST_PREFIX "SemanticStorageControllerTest.")
alcedo_copy_duckdb_extensions(SemanticStorageControllerTest)
alcedo_register_test_target(SemanticStorageControllerTest storage)
