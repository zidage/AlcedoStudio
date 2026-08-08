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
gtest_discover_tests(DuckormExprTest TEST_PREFIX "DuckormExprTest.")
alcedo_register_test_target(DuckormExprTest storage)

# Compile-path unit checks for FilterNode -> SqlFragment (no folder fixtures).
add_executable(SleeveFilterCompileTest ${ALCEDO_TEST_ROOT}/sleeve/sleeve_filter_compile_test.cpp)
target_include_directories(SleeveFilterCompileTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(SleeveFilterCompileTest PRIVATE GTest::gtest_main SleeveFilter StrConv)
gtest_discover_tests(SleeveFilterCompileTest TEST_PREFIX "SleeveFilterCompileTest.")
alcedo_register_test_target(SleeveFilterCompileTest storage)

# Compile-path unit checks for the sleeve filter factories (stats-bar bucket
# predicates, semantic-label EXISTS, merge/compile entry helpers).
add_executable(SleeveFilterFactoryTest ${ALCEDO_TEST_ROOT}/sleeve/sleeve_filter_factory_test.cpp)
target_include_directories(SleeveFilterFactoryTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(SleeveFilterFactoryTest PRIVATE GTest::gtest_main SleeveFilter StrConv)
gtest_discover_tests(SleeveFilterFactoryTest TEST_PREFIX "SleeveFilterFactoryTest.")
alcedo_register_test_target(SleeveFilterFactoryTest storage)
