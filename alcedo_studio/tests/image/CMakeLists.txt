# Image domain test target ownership.
include("${ALCEDO_TEST_ROOT}/cmake/AlcedoTestRegistration.cmake")

add_executable(MetadataExtractorTest ${ALCEDO_TEST_ROOT}/image/metadata_extractor_test.cpp)
target_include_directories(MetadataExtractorTest PUBLIC ${ALCEDO_INCLUDE_DIR} ${ALCEDO_TEST_DIR})
target_link_libraries(MetadataExtractorTest PRIVATE GTest::gtest_main Image EditGraph
    OpenImageIO::OpenImageIO)
gtest_discover_tests(MetadataExtractorTest TEST_PREFIX "MetadataExtractorTest.")
alcedo_register_test_target(MetadataExtractorTest image)

add_executable(BatchImportDngMetadataTest ${ALCEDO_TEST_ROOT}/image/batch_import_dng_metadata_test.cpp)
target_include_directories(BatchImportDngMetadataTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(BatchImportDngMetadataTest PRIVATE GTest::gtest_main Image)
gtest_discover_tests(BatchImportDngMetadataTest TEST_PREFIX "BatchImportDngMetadataTest.")
alcedo_register_test_target(BatchImportDngMetadataTest image)

# Runtime DNG profile cache: LRU eviction, sharing by fingerprint, reload after file change.
add_executable(DngColorProfileCacheTest ${ALCEDO_TEST_ROOT}/image/dng_color_profile_cache_test.cpp)
target_include_directories(DngColorProfileCacheTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(DngColorProfileCacheTest PRIVATE GTest::gtest_main Image)
gtest_discover_tests(DngColorProfileCacheTest TEST_PREFIX "DngColorProfileCacheTest."
  PROPERTIES LABELS "ci_core_flow;image")
alcedo_register_test_target(DngColorProfileCacheTest image ci_core)

if(ALCEDO_METAL_ENABLED)
  add_executable(ImageBufferMetalTest ${ALCEDO_TEST_ROOT}/image/image_buffer_metal_test.cpp)
  target_include_directories(ImageBufferMetalTest PUBLIC ${ALCEDO_INCLUDE_DIR})
  target_link_libraries(ImageBufferMetalTest PRIVATE GTest::gtest_main ImageBuffer MetalImage ${ALCEDO_TEST_OPENCV_MIN_DEPS})
  gtest_discover_tests(ImageBufferMetalTest TEST_PREFIX "ImageBufferMetalTest.")
  alcedo_register_test_target(ImageBufferMetalTest image)

  add_executable(MetalGeometryUtilsTest ${ALCEDO_TEST_ROOT}/metal/metal_geometry_utils_test.cpp)
  target_include_directories(MetalGeometryUtilsTest PUBLIC ${ALCEDO_INCLUDE_DIR})
  target_link_libraries(MetalGeometryUtilsTest PRIVATE GTest::gtest_main MetalGeometryUtils
                                                       MetalImage ${ALCEDO_TEST_OPENCV_MIN_DEPS})
  gtest_discover_tests(MetalGeometryUtilsTest TEST_PREFIX "MetalGeometryUtilsTest.")
  alcedo_register_test_target(MetalGeometryUtilsTest image gpu)
endif()
