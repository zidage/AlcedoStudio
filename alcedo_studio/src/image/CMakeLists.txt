# Core Image Handling target ownership.
# 2. Core Image Handling 
# ==============================================================================

# ImageBuffer (Platform specific deps handling)
set(IMAGE_BUFFER_SRCS ${ALCEDO_SRC_ROOT}/image/image_buffer.cpp)
if(ALCEDO_OPENCL_ENABLED)
    list(APPEND IMAGE_BUFFER_SRCS ${ALCEDO_SRC_ROOT}/image/opencl_image.cpp)
endif()
add_library(ImageBuffer ${IMAGE_BUFFER_SRCS})
target_include_directories(ImageBuffer PUBLIC "${ALCEDO_INCLUDE_ROOT}")

# Common dependencies
set(IMAGE_BUFFER_DEPS Exiv2 ${ALCEDO_OPENCV_CORE_DEPS} libraw::raw_r TimeProvider JSON StrConv)

# Platform specific dependencies
if (WIN32)
    target_link_libraries(ImageBuffer PUBLIC ${IMAGE_BUFFER_DEPS} xxHash::xxhash)
elseif(ALCEDO_METAL_ENABLED)
    target_link_libraries(ImageBuffer PUBLIC ${IMAGE_BUFFER_DEPS} xxHash MetalImage MetalUtils)
else()
    target_link_libraries(ImageBuffer PUBLIC ${IMAGE_BUFFER_DEPS} xxHash)
endif()
if(ALCEDO_OPENCL_ENABLED)
    target_link_libraries(ImageBuffer PUBLIC OpenClContext)
endif()

if(ALCEDO_OPENCL_ENABLED)
    def_library(OpenClGeometryUtils
        SOURCES ${ALCEDO_SRC_ROOT}/opencl/opencl_geometry_utils.cpp
        PUBLIC_DEPS ImageBuffer OpenClProgramLibrary ${ALCEDO_OPENCV_CORE_DEPS}
    )
    target_compile_definitions(OpenClGeometryUtils PUBLIC HAVE_OPENCL)

    # Reusable OpenCL NN primitives (device buffer, workspace, NHWC4 conv).
    # No Bayer/X-Trans topology, model paths, or RAW routing — product modules only.
    def_library(OpenClNn
        SOURCES
            ${ALCEDO_SRC_ROOT}/opencl/nn/device_buffer.cpp
            ${ALCEDO_SRC_ROOT}/opencl/nn/activation_slots.cpp
            ${ALCEDO_SRC_ROOT}/opencl/nn/workspace.cpp
            ${ALCEDO_SRC_ROOT}/opencl/nn/convolution.cpp
            ${ALCEDO_SRC_ROOT}/opencl/nn/demosaicnet_stage_profiler.cpp
        PUBLIC_DEPS
            OpenClContext
            OpenClProgramLibrary
            OpenCL::OpenCL
    )
    target_compile_definitions(OpenClNn PUBLIC HAVE_OPENCL)

    # Fixed OpenCL DemosaicNet modules and lazy model cache.
    def_library(OpenClDemosaicNet
        SOURCES
            ${ALCEDO_SRC_ROOT}/decoders/processor/nn/opencl_demosaicnet_module.cpp
            ${ALCEDO_SRC_ROOT}/decoders/processor/nn/opencl_demosaicnet_cache.cpp
            ${ALCEDO_SRC_ROOT}/decoders/processor/nn/opencl_demosaicnet_tiled.cpp
        PUBLIC_DEPS
            OpenClNn
            OpenClProgramLibrary
            NnSafetensors
            OpenCL::OpenCL
            ${ALCEDO_OPENCV_CORE_DEPS}
    )
    target_compile_definitions(OpenClDemosaicNet PUBLIC HAVE_OPENCL)
    target_compile_definitions(OpenClDemosaicNet PRIVATE
        ALCEDO_DEMOASICNET_MODEL_DIR="${ALCEDO_SRC_ROOT}/config/models"
    )

    # Product-path OpenCL Neural demosaic entry. Soft-fail API for
    # RawProcessor OpenCL branch; not part of the generic OpenClDemosaicNet module layer.
    def_library(OpenClDemosaicNetEntry
        SOURCES
            ${ALCEDO_SRC_ROOT}/decoders/processor/operators/gpu/opencl_demosaicnet.cpp
        PUBLIC_DEPS
            OpenClDemosaicNet
            ImageBuffer
            OpenClProgramLibrary
            OpenCL::OpenCL
            ${ALCEDO_OPENCV_CORE_DEPS}
    )
    target_compile_definitions(OpenClDemosaicNetEntry PUBLIC HAVE_OPENCL)
endif()

# Image Base
def_library(Image
    SOURCES ${ALCEDO_SRC_ROOT}/image/image.cpp 
    ${ALCEDO_SRC_ROOT}/image/metadata_extractor.cpp
    PUBLIC_DEPS ImageBuffer
)
target_link_libraries(Image PRIVATE OpenImageIO::OpenImageIO)

# Image Pool
def_library(ImagePool
    SOURCES ${ALCEDO_SRC_ROOT}/storage/image_pool/image_pool_manager.cpp
    PRIVATE_DEPS Image
)

# ==============================================================================
