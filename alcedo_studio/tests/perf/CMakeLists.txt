# Performance harness test target ownership.

include("${ALCEDO_TEST_ROOT}/cmake/AlcedoTestRegistration.cmake")

if(ALCEDO_CUDA_ENABLED)
  # Phase 8.1: standalone Neural/Legacy full-frame and tile/conv diagnosis harness.
  # Not part of default ctest. Build Release for authoritative timings:
  #   cmd /c scripts\msvc_env.cmd --build --preset win_release --target DemosaicNetPerfHarness --parallel 4
  execute_process(
    COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE ALCEDO_GIT_COMMIT_SHORT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
  )
  if(NOT ALCEDO_GIT_COMMIT_SHORT)
    set(ALCEDO_GIT_COMMIT_SHORT "unknown")
  endif()

  add_executable(DemosaicNetPerfHarness
    ${ALCEDO_TEST_ROOT}/perf/demosaicnet_perf_harness.cpp
    ${ALCEDO_TEST_ROOT}/perf/demosaicnet_perf_metrics.cpp
    ${ALCEDO_TEST_ROOT}/perf/demosaicnet_perf_roofline.cpp
  )
  target_include_directories(DemosaicNetPerfHarness PRIVATE
    ${ALCEDO_INCLUDE_DIR}
    ${ALCEDO_TEST_ROOT}/perf
  )
  target_link_libraries(DemosaicNetPerfHarness PRIVATE
    RawProcessor
    CudaDemosaicNetEntry
    DemosaicNet
    CudaUtils
    CUDA::cudart
    ${ALCEDO_TEST_OPENCV_MIN_DEPS}
    opencv_core
  )
  set_target_properties(DemosaicNetPerfHarness PROPERTIES CUDA_SEPARABLE_COMPILATION ON)
  target_compile_definitions(DemosaicNetPerfHarness PRIVATE
    ALCEDO_DEMOASICNET_MODEL_DIR="${CMAKE_SOURCE_DIR}/alcedo_studio/src/config/models"
    ALCEDO_GIT_COMMIT="${ALCEDO_GIT_COMMIT_SHORT}"
  )
  # Prefer Release for authoritative numbers; still buildable in Debug for bring-up.
  if(ALCEDO_ENABLE_GPU_PERF_TESTS)
    add_test(
      NAME DemosaicNetPerfHarness.FullBayerBoth
      COMMAND $<TARGET_FILE:DemosaicNetPerfHarness>
        --fixture bayer
        --method both
        --mode full
        --warmup 3
        --iterations 20
        --output ${CMAKE_BINARY_DIR}/perf/demosaicnet_bayer.json
    )
    set_tests_properties(DemosaicNetPerfHarness.FullBayerBoth PROPERTIES
      LABELS "performance"
      TIMEOUT 7200
    )
    add_test(
      NAME DemosaicNetPerfHarness.FullXTransBoth
      COMMAND $<TARGET_FILE:DemosaicNetPerfHarness>
        --fixture xtrans
        --method both
        --mode full
        --warmup 3
        --iterations 20
        --output ${CMAKE_BINARY_DIR}/perf/demosaicnet_xtrans.json
    )
    set_tests_properties(DemosaicNetPerfHarness.FullXTransBoth PROPERTIES
      LABELS "performance"
      TIMEOUT 14400
    )
  endif()
  alcedo_register_test_target(DemosaicNetPerfHarness gpu)
endif()
