# RAW decode and RAW GPU operator test target ownership.

include("${ALCEDO_TEST_ROOT}/cmake/AlcedoTestRegistration.cmake")

# --- Unconditional raw targets ---

add_executable(SingleRawLoad ${ALCEDO_TEST_ROOT}/raw/single_raw_decode_test.cpp)
target_include_directories(SingleRawLoad PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(SingleRawLoad PRIVATE GTest::gtest_main libraw::raw ${ALCEDO_TEST_OPENCV_MIN_DEPS})
target_compile_definitions(SingleRawLoad PRIVATE LIBRAW_NODLL)
alcedo_register_test_target(SingleRawLoad raw)

add_executable(SingleThumbnailLoad ${ALCEDO_TEST_ROOT}/raw/single_raw_thumbnail_test.cpp)
target_include_directories(SingleThumbnailLoad PUBLIC ${ALCEDO_INCLUDE_DIR})
target_link_libraries(SingleThumbnailLoad PRIVATE GTest::gtest_main libraw::raw ${ALCEDO_TEST_OPENCV_MIN_DEPS})
target_compile_definitions(SingleThumbnailLoad PRIVATE LIBRAW_NODLL)
alcedo_register_test_target(SingleThumbnailLoad raw)

if(ALCEDO_METAL_ENABLED)
  add_executable(MetalRawOpsTest ${ALCEDO_TEST_ROOT}/raw/metal_raw_ops_test.cpp)
  target_include_directories(MetalRawOpsTest PUBLIC ${ALCEDO_INCLUDE_DIR})
  target_link_libraries(MetalRawOpsTest PRIVATE GTest::gtest_main RawProcessorOp
                                                MetalImage MetalContext ${ALCEDO_TEST_OPENCV_MIN_DEPS})
  gtest_discover_tests(MetalRawOpsTest TEST_PREFIX "MetalRawOpsTest.")
  alcedo_register_test_target(MetalRawOpsTest raw gpu)
endif()

# --- CUDA-guarded raw targets ---

if(ALCEDO_CUDA_ENABLED AND ALCEDO_ENABLE_CUDA_IMAGE_GEOMETRY_OPS_TEST)
  add_executable(CudaImageGeometryOpsTest ${ALCEDO_TEST_ROOT}/raw/cuda_image_geometry_ops_test.cpp)
  target_include_directories(CudaImageGeometryOpsTest PUBLIC ${ALCEDO_INCLUDE_DIR})
  target_link_libraries(CudaImageGeometryOpsTest PRIVATE GTest::gtest_main RawProcessorOp EditRuntimeLens ${ALCEDO_TEST_OPENCV_MIN_DEPS})
  gtest_discover_tests(CudaImageGeometryOpsTest TEST_PREFIX "CudaImageGeometryOpsTest.")
  alcedo_register_test_target(CudaImageGeometryOpsTest raw gpu)
endif()

if(ALCEDO_CUDA_ENABLED)
  add_executable(CudaRawOpsTest ${ALCEDO_TEST_ROOT}/raw/cuda_raw_ops_test.cpp)
  target_include_directories(CudaRawOpsTest PUBLIC ${ALCEDO_INCLUDE_DIR})
  target_link_libraries(CudaRawOpsTest PRIVATE GTest::gtest_main RawProcessorOp
                                               CudaDemosaicNetEntry CUDA::cudart
                                               ${ALCEDO_TEST_OPENCV_MIN_DEPS})
  gtest_discover_tests(CudaRawOpsTest TEST_PREFIX "CudaRawOpsTest.")
  alcedo_register_test_target(CudaRawOpsTest raw gpu)
endif()

# --- OpenCL + CUDA guarded raw targets ---

if(ALCEDO_CUDA_ENABLED AND ALCEDO_OPENCL_ENABLED)
  add_executable(OpenClCudaFullPipelineBenchmark ${ALCEDO_TEST_ROOT}/raw/opencl_cuda_full_pipeline_benchmark.cpp)
  target_include_directories(OpenClCudaFullPipelineBenchmark PUBLIC ${ALCEDO_INCLUDE_DIR})
  target_link_libraries(OpenClCudaFullPipelineBenchmark PRIVATE GTest::gtest_main OpenClProgramLibrary AcceleratorBackend EditPipeline PipelineScheduler ThreadPool ${ALCEDO_TEST_OPENCV_MIN_DEPS})
  gtest_discover_tests(OpenClCudaFullPipelineBenchmark TEST_PREFIX "OpenClCudaBenchmark.")
  alcedo_register_test_target(OpenClCudaFullPipelineBenchmark raw gpu)
endif()
