# Color encoding catalog test target ownership (lut_color_encoding_plan.md, Phase L1).
include("${ALCEDO_TEST_ROOT}/cmake/AlcedoTestRegistration.cmake")

# Curves against OpenColorIO 2.5.1 builtins or vendor code values, gamut matrices against OCIO,
# round trips and the encoding table. Only test targets link OpenColorIO.
add_executable(ColorEncodingCatalogTest ${ALCEDO_TEST_ROOT}/color/color_encoding_catalog_test.cpp)
target_include_directories(ColorEncodingCatalogTest PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(ColorEncodingCatalogTest PRIVATE GTest::gtest_main ColorEncodingCatalog
  OpenColorIO::OpenColorIO)
gtest_discover_tests(ColorEncodingCatalogTest TEST_PREFIX "ColorEncodingCatalogTest."
  PROPERTIES LABELS "color")
alcedo_register_test_target(ColorEncodingCatalogTest core)

if(ALCEDO_CUDA_ENABLED)
  add_executable(ColorEncodingCudaTest ${ALCEDO_TEST_ROOT}/color/color_encoding_cuda_test.cu)
  target_include_directories(ColorEncodingCudaTest PUBLIC ${ALCEDO_INCLUDE_DIR})
  target_link_libraries(ColorEncodingCudaTest PRIVATE GTest::gtest_main CUDA::cudart)
  gtest_discover_tests(ColorEncodingCudaTest TEST_PREFIX "ColorEncodingCudaTest."
    PROPERTIES LABELS "gpu;color")
  alcedo_register_test_target(ColorEncodingCudaTest gpu)
endif()

if(ALCEDO_OPENCL_ENABLED)
  add_executable(ColorEncodingOpenClTest ${ALCEDO_TEST_ROOT}/color/color_encoding_opencl_test.cpp)
  target_include_directories(ColorEncodingOpenClTest PUBLIC ${ALCEDO_INCLUDE_DIR})
  target_link_libraries(ColorEncodingOpenClTest PRIVATE GTest::gtest_main OpenCL::OpenCL)
  target_compile_definitions(ColorEncodingOpenClTest PRIVATE
    ALCEDO_COLOR_ENCODING_MATH_SOURCE_PATH="${ALCEDO_INCLUDE_DIR}/color/color_encoding_math.h")
  gtest_discover_tests(ColorEncodingOpenClTest TEST_PREFIX "ColorEncodingOpenClTest."
    PROPERTIES LABELS "gpu;opencl;color")
  alcedo_register_test_target(ColorEncodingOpenClTest gpu)
endif()
