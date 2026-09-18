# ==============================================================================
# Build-context roots (Phase 3A). Immutable for child manifests; set before any
# first-party test add_subdirectory. Legacy ALCEDO_*_DIR aliases keep existing
# path references stable until domain manifests adopt ALCEDO_TEST_ROOT.
# ==============================================================================
set(ALCEDO_TEST_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
set(ALCEDO_TEST_SUPPORT_ROOT "${ALCEDO_TEST_ROOT}/support")
set(ALCEDO_ROOT_DIR "${ALCEDO_TEST_ROOT}/..")
set(ALCEDO_SRC_DIR "${ALCEDO_ROOT_DIR}/src")
set(ALCEDO_INCLUDE_DIR "${ALCEDO_SRC_DIR}/include")
set(ALCEDO_TEST_DIR "${ALCEDO_TEST_ROOT}")

# Not used by now, but kept for future test cases
add_library(MemLeakDect INTERFACE leak_detector/memory_leak_detector.hpp)
target_link_libraries(MemLeakDect INTERFACE GTest::gtest_main)

set(ALCEDO_TEST_OPENCV_MIN_DEPS
    opencv_core
    opencv_imgproc
    opencv_imgcodecs
)

include(GoogleTest)
set(CMAKE_GTEST_DISCOVER_TESTS_DISCOVERY_MODE PRE_TEST)

option(ALCEDO_BUILD_TESTS_BY_DEFAULT
       "Build enabled test targets as part of the default all target." OFF)
option(ALCEDO_BUILD_CORE_TESTS "Enable small core/unit test category targets." ON)
option(ALCEDO_BUILD_IMAGE_TESTS "Enable image and ImageBuffer test category targets." ON)
option(ALCEDO_BUILD_IO_TESTS "Enable IO/export/metadata test category targets." ON)
option(ALCEDO_BUILD_RAW_TESTS "Enable RAW decode/processor test category targets." ON)
option(ALCEDO_BUILD_GPU_TESTS "Enable CUDA/Metal GPU test category targets." ON)
option(ALCEDO_BUILD_EDIT_TESTS "Enable edit operator and pipeline test category targets." ON)
option(ALCEDO_BUILD_UI_TESTS "Enable Qt/UI test category targets." ON)
option(ALCEDO_BUILD_APP_TESTS "Enable application service test category targets." ON)
option(ALCEDO_BUILD_STORAGE_TESTS "Enable storage/Sleeve test category targets." ON)
option(ALCEDO_BUILD_DEMO_TARGETS "Enable GUI/demo target category targets." OFF)
option(ALCEDO_BUILD_CI_TESTS "Enable zero-extra-cost GitHub CI test targets." OFF)
option(ALCEDO_ENABLE_CUDA_IMAGE_GEOMETRY_OPS_TEST
       "Enable CudaImageGeometryOpsTest discovery and execution." ON)
option(ALCEDO_ENABLE_CUDA_PREVIEW_VRAM_RECLAMATION_TEST
       "Enable CudaPreviewVramReclamationTest discovery and execution." ON)
option(ALCEDO_ENABLE_GPU_PERF_TESTS
       "Register GPU performance harnesses with the performance ctest label (opt-in)." OFF)

add_custom_target(alcedo_tests_all)
add_custom_target(alcedo_tests_core)
add_custom_target(alcedo_tests_image)
add_custom_target(alcedo_tests_io)
add_custom_target(alcedo_tests_raw)
add_custom_target(alcedo_tests_gpu)
add_custom_target(alcedo_tests_edit)
add_custom_target(alcedo_tests_ui)
add_custom_target(alcedo_tests_app)
add_custom_target(alcedo_tests_storage)
add_custom_target(alcedo_tests_demos)
add_custom_target(alcedo_tests_ci_core)
add_custom_target(alcedo_tests_ci_raw)
add_custom_target(alcedo_tests_ci_metal)

include("${ALCEDO_TEST_ROOT}/cmake/AlcedoTestRegistration.cmake")

# ==============================================================================
# Phase 3D test domain manifests
# ==============================================================================
add_subdirectory(utils)
add_subdirectory(ui)
add_subdirectory(sleeve)
add_subdirectory(raw)
add_subdirectory(perf)
add_subdirectory(ml_ops)
add_subdirectory(io)
add_subdirectory(image)
add_subdirectory(edit)
add_subdirectory(cuda)
add_subdirectory(ci)
add_subdirectory(app)

# Category aggregate membership is registered via alcedo_register_test_target
# in each domain manifest. The root keeps infrastructure only.
#
# Windows lensfun/duckdb test runtime fixup is scheduled automatically when a
# domain includes AlcedoTestRegistration.cmake (or calls
# alcedo_register_test_target). Do not hand-roll POST_BUILD DLL copies in domain
# manifests — that opt-in pattern is why 0xC0000139 kept returning for new tests.


add_test(NAME PackagedNoticeSourcesContainQuickQanavaAndBezierClauses
    COMMAND ${CMAKE_COMMAND}
        -DALCEDO_SOURCE_DIR=${CMAKE_SOURCE_DIR}
        -P ${CMAKE_SOURCE_DIR}/scripts/cmake/check_quickqanava_legal_notices.cmake
)
set_tests_properties(PackagedNoticeSourcesContainQuickQanavaAndBezierClauses PROPERTIES
    LABELS "quickqanava;legal_notices"
)
