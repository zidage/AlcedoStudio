# IO domain test target ownership.
include("${ALCEDO_TEST_ROOT}/cmake/AlcedoTestRegistration.cmake")

add_executable(ImageWriterTest ${ALCEDO_TEST_ROOT}/io/image_writer_test.cpp)
target_include_directories(ImageWriterTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(ImageWriterTest PRIVATE GTest::gtest_main ImageWriter ${ALCEDO_TEST_OPENCV_MIN_DEPS})
gtest_discover_tests(ImageWriterTest TEST_PREFIX "ImageWriterTest."
  PROPERTIES LABELS "ci_core_flow")
alcedo_register_test_target(ImageWriterTest io ci_core)

add_executable(ExportIccProfileResolverTest ${ALCEDO_TEST_ROOT}/io/export_icc_profile_resolver_test.cpp)
target_include_directories(ExportIccProfileResolverTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(ExportIccProfileResolverTest PRIVATE GTest::gtest_main ExportIccProfileResolver)
gtest_discover_tests(ExportIccProfileResolverTest TEST_PREFIX "ExportIccProfileResolverTest.")
alcedo_register_test_target(ExportIccProfileResolverTest io)

add_executable(ImagePathEncodingTest ${ALCEDO_TEST_ROOT}/io/image_path_encoding_test.cpp)
target_include_directories(ImagePathEncodingTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(ImagePathEncodingTest PRIVATE GTest::gtest_main ImageLoader StorageService)
gtest_discover_tests(ImagePathEncodingTest TEST_PREFIX "ImagePathEncodingTest.")
alcedo_register_test_target(ImagePathEncodingTest io)
