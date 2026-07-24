# Sleeve / storage test target ownership.
include("${ALCEDO_TEST_ROOT}/cmake/AlcedoTestRegistration.cmake")

add_executable(SleeveFSTest ${ALCEDO_TEST_ROOT}/sleeve/sleeve_fs_test.cpp)
target_include_directories(SleeveFSTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(SleeveFSTest PRIVATE GTest::gtest_main SleeveFS StrConv)
gtest_discover_tests(SleeveFSTest TEST_PREFIX "SleeveFSTest.")
alcedo_register_test_target(SleeveFSTest storage)
