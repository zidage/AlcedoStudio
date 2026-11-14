# Image domain test target ownership.
include("${ALCEDO_TEST_ROOT}/cmake/AlcedoTestRegistration.cmake")

add_executable(MetadataExtractorTest ${ALCEDO_TEST_ROOT}/image/metadata_extractor_test.cpp)
target_include_directories(MetadataExtractorTest PUBLIC ${ALCEDO_INCLUDE_DIR} ${ALCEDO_TEST_DIR})
target_link_libraries(MetadataExtractorTest PRIVATE GTest::gtest_main Image EditGraph
    OpenImageIO::OpenImageIO)
target_compile_definitions(MetadataExtractorTest PRIVATE
    ALCEDO_RASTER_FIXTURE_DIR="${ALCEDO_TEST_ROOT}/resources/raster")
gtest_discover_tests(MetadataExtractorTest TEST_PREFIX "MetadataExtractorTest.")
alcedo_register_test_target(MetadataExtractorTest image)

add_executable(BatchImportDngMetadataTest ${ALCEDO_TEST_ROOT}/image/batch_import_dng_metadata_test.cpp)
target_include_directories(BatchImportDngMetadataTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(BatchImportDngMetadataTest PRIVATE GTest::gtest_main Image)
gtest_discover_tests(BatchImportDngMetadataTest TEST_PREFIX "BatchImportDngMetadataTest.")
alcedo_register_test_target(BatchImportDngMetadataTest image)

# Source color description of JPEG, PNG, TIFF and OpenEXR files (raster_image_input_plan.md).
add_executable(RasterColorDescriptionTest
  ${ALCEDO_TEST_ROOT}/image/raster_color_description_test.cpp)
target_include_directories(RasterColorDescriptionTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(RasterColorDescriptionTest PRIVATE GTest::gtest_main Image Sha256Digest)
target_compile_definitions(RasterColorDescriptionTest PRIVATE
  ALCEDO_RASTER_FIXTURE_DIR="${ALCEDO_TEST_ROOT}/resources/raster")
gtest_discover_tests(RasterColorDescriptionTest TEST_PREFIX "RasterColorDescriptionTest.")
alcedo_register_test_target(RasterColorDescriptionTest image)

# Runtime DNG profile cache: LRU eviction, sharing by fingerprint, reload after file change.
add_executable(DngColorProfileCacheTest ${ALCEDO_TEST_ROOT}/image/dng_color_profile_cache_test.cpp)
target_include_directories(DngColorProfileCacheTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(DngColorProfileCacheTest PRIVATE GTest::gtest_main Image)
gtest_discover_tests(DngColorProfileCacheTest TEST_PREFIX "DngColorProfileCacheTest."
  PROPERTIES LABELS "ci_core_flow;image")
alcedo_register_test_target(DngColorProfileCacheTest image ci_core)

# DCP file reader: synthetic DCP files from dcp_test_file_writer.hpp, no vendor profile bytes.
add_executable(DcpFileReaderTest ${ALCEDO_TEST_ROOT}/image/dcp_file_reader_test.cpp)
target_include_directories(DcpFileReaderTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(DcpFileReaderTest PRIVATE GTest::gtest_main Image)
gtest_discover_tests(DcpFileReaderTest TEST_PREFIX "DcpFileReaderTest."
  PROPERTIES LABELS "ci_core_flow;image")
alcedo_register_test_target(DcpFileReaderTest image ci_core)

# ProfileToneCurve spline, table, and baseline RGB tone against a port of the DNG SDK.
add_executable(DngToneCurveTest ${ALCEDO_TEST_ROOT}/image/dng_tone_curve_test.cpp)
target_include_directories(DngToneCurveTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(DngToneCurveTest PRIVATE GTest::gtest_main Image)
gtest_discover_tests(DngToneCurveTest TEST_PREFIX "DngToneCurveTest."
  PROPERTIES LABELS "ci_core_flow;image")
alcedo_register_test_target(DngToneCurveTest image ci_core)

# DCP library index, camera match, and loaded-profile cache over synthetic DCP files.
add_executable(DcpProfileLibraryTest ${ALCEDO_TEST_ROOT}/image/dcp_profile_library_test.cpp)
target_include_directories(DcpProfileLibraryTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(DcpProfileLibraryTest PRIVATE GTest::gtest_main Image)
gtest_discover_tests(DcpProfileLibraryTest TEST_PREFIX "DcpProfileLibraryTest."
  PROPERTIES LABELS "ci_core_flow;image")
alcedo_register_test_target(DcpProfileLibraryTest image ci_core)

# Exiv2 XMP toolkit started on the main thread before parallel XMP decodes (issue 389). Its own
# executable, so no other test in the process starts the toolkit first.
add_executable(Exiv2XmpToolkitScopeTest ${ALCEDO_TEST_ROOT}/image/exiv2_xmp_toolkit_scope_test.cpp)
target_include_directories(Exiv2XmpToolkitScopeTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(Exiv2XmpToolkitScopeTest PRIVATE GTest::gtest_main Image)
gtest_discover_tests(Exiv2XmpToolkitScopeTest TEST_PREFIX "Exiv2XmpToolkitScopeTest.")
alcedo_register_test_target(Exiv2XmpToolkitScopeTest image)

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
