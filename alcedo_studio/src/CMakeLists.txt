# ==============================================================================
# Build-context roots (Phase 3A). Immutable for child manifests; set before any
# first-party add_subdirectory. ALCEDO_INCLUDE_ROOT is the stable public header
# root used by def_library (never a directory-relative "include").
# ==============================================================================
set(ALCEDO_SRC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
set(ALCEDO_INCLUDE_ROOT "${ALCEDO_SRC_ROOT}/include")
set(ALCEDO_BINARY_ROOT "${CMAKE_CURRENT_BINARY_DIR}")

include("${ALCEDO_SRC_ROOT}/cmake/AlcedoTargetHelpers.cmake")

# ==============================================================================
# 1. Utilities
# ==============================================================================

set(ALCEDO_OPENCV_CORE_DEPS
    opencv_core
)
set(ALCEDO_OPENCV_IMAGE_DEPS
    opencv_core
    opencv_imgproc
    opencv_imgcodecs
    opencv_highgui
)

# ==============================================================================
# Phase 3D composition roots — domain manifests
# ==============================================================================
add_subdirectory(concurrency)
add_subdirectory(utils)
add_subdirectory(nn)


add_subdirectory(opencl)


add_subdirectory(cuda)

add_subdirectory(metal)

# ==============================================================================
add_subdirectory(image)
add_subdirectory(decoders)
add_subdirectory(edit)

add_subdirectory(sleeve)
add_subdirectory(storage)

add_subdirectory(io)



add_subdirectory(sidecar_client)
add_subdirectory(app)

add_subdirectory(ui)
