# AI Denoise test target ownership
# (docs/roadmap/alcedo_studio/raw-processing/ai_denoise/ai_denoise_integration_plan.md).

include("${ALCEDO_TEST_ROOT}/cmake/AlcedoTestRegistration.cmake")

# DN3: the overlap tile planner and the tile loop with a test session and a test clock (all
# platforms, no assets).
add_executable(UtNet2TileLoopTest
  ${ALCEDO_TEST_ROOT}/ai_denoise/utnet2_tile_loop_test.cpp
)
target_include_directories(UtNet2TileLoopTest PRIVATE ${ALCEDO_INCLUDE_DIR})
target_link_libraries(UtNet2TileLoopTest PRIVATE GTest::gtest_main UtNet2TileLoop)
gtest_discover_tests(UtNet2TileLoopTest TEST_PREFIX "UtNet2TileLoopTest.")
alcedo_register_test_target(UtNet2TileLoopTest core ci_core)

# DN2: the ONNX Runtime DirectML session of the FP16 UtNet2 graph (Windows x64). The tests read
# the graph and the DN0 expected outputs from the folder of ALCEDO_AI_DENOISE_TEST_ASSETS and skip
# without it.
if(TARGET UtNet2DirectMl)
  add_executable(UtNet2DirectMlTest
    ${ALCEDO_TEST_ROOT}/ai_denoise/utnet2_directml_session_test.cpp
  )
  target_include_directories(UtNet2DirectMlTest PRIVATE ${ALCEDO_INCLUDE_DIR})
  target_link_libraries(UtNet2DirectMlTest PRIVATE GTest::gtest_main UtNet2DirectMl NnSafetensors)
  target_compile_definitions(UtNet2DirectMlTest PRIVATE
    ALCEDO_TEST_TEMP_ROOT="${CMAKE_SOURCE_DIR}/build/tmp/utnet2-directml-test")
  alcedo_copy_onnxruntime_directml_runtime(UtNet2DirectMlTest)
  gtest_discover_tests(UtNet2DirectMlTest TEST_PREFIX "UtNet2DirectMlTest.")
  alcedo_register_test_target(UtNet2DirectMlTest gpu)

  # DN3: the photo timing tool (not a test; it needs the packed photos of the lab).
  add_executable(UtNet2PhotoTimingTool
    ${ALCEDO_TEST_ROOT}/ai_denoise/utnet2_photo_timing_tool.cpp
  )
  target_include_directories(UtNet2PhotoTimingTool PRIVATE ${ALCEDO_INCLUDE_DIR})
  target_link_libraries(UtNet2PhotoTimingTool PRIVATE UtNet2DirectMl dxgi)
  alcedo_copy_onnxruntime_directml_runtime(UtNet2PhotoTimingTool)
endif()
