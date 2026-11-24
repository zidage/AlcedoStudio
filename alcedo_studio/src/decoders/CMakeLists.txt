# Decoders target ownership (RAW GPU operators, DemosaicNet, image decoding).
# 3. Processors & Operators
# ==============================================================================

# --- RawProcessorOp: RAW GPU operators used by the GPU DAG develop passes ---
set(RAW_OP_GPU_SRCS)
if(ALCEDO_CUDA_ENABLED)
    list(APPEND RAW_OP_GPU_SRCS
        ${ALCEDO_SRC_ROOT}/decoders/processor/operators/gpu/cuda_color_space_conv.cu
        ${ALCEDO_SRC_ROOT}/decoders/processor/operators/gpu/cuda_debayer_rcd.cu
        ${ALCEDO_SRC_ROOT}/decoders/processor/operators/gpu/cuda_dng_warp.cu
        ${ALCEDO_SRC_ROOT}/decoders/processor/operators/gpu/cuda_highlight_reconstruct.cu
        ${ALCEDO_SRC_ROOT}/decoders/processor/operators/gpu/cuda_image_ops.cu
        ${ALCEDO_SRC_ROOT}/decoders/processor/operators/gpu/cuda_white_balance.cu
        ${ALCEDO_SRC_ROOT}/decoders/processor/operators/gpu/cuda_xtrans_interpolate.cu
    )
endif()
if(ALCEDO_METAL_ENABLED)
    list(APPEND RAW_OP_GPU_SRCS
        ${ALCEDO_SRC_ROOT}/decoders/processor/operators/gpu/metal_encode.cpp
    )
endif()
if(ALCEDO_OPENCL_ENABLED)
    list(APPEND RAW_OP_GPU_SRCS
        ${ALCEDO_SRC_ROOT}/decoders/processor/operators/gpu/opencl_encode.cpp
    )
endif()

# RawProcessorOp Target
def_cuda_library(RawProcessorOp
    SOURCES ${RAW_OP_GPU_SRCS}
    PUBLIC_DEPS ImageBuffer libraw::raw_r Exiv2 hwy::hwy Eigen3::Eigen
)
target_link_libraries(RawProcessorOp PRIVATE OpenMP::OpenMP_CXX)

if(ALCEDO_CUDA_ENABLED)
    set_target_properties(RawProcessorOp PROPERTIES CUDA_SEPARABLE_COMPILATION ON)
    set_target_properties(RawProcessorOp PROPERTIES CUDA_RESOLVE_DEVICE_SYMBOLS ON)
    target_compile_definitions(RawProcessorOp PUBLIC HAVE_CUDA)
endif()
if(ALCEDO_METAL_ENABLED)
    add_dependencies(RawProcessorOp RawProcessorOpMetalShaders MetalGeometryUtilsShaders)
    target_link_libraries(RawProcessorOp PRIVATE MetalGeometryUtils MetalImage MetalContext)
    target_compile_definitions(RawProcessorOp PRIVATE
        ALCEDO_METAL_TO_LINEAR_REF_METALLIB_PATH="${METAL_TO_LINEAR_REF_METALLIB}"
        ALCEDO_METAL_DEBAYER_RCD_METALLIB_PATH="${METAL_DEBAYER_RCD_METALLIB}"
        ALCEDO_METAL_HIGHLIGHT_RECONSTRUCT_METALLIB_PATH="${METAL_HIGHLIGHT_RECONSTRUCT_METALLIB}"
        ALCEDO_METAL_XTRANS_INTERPOLATE_METALLIB_PATH="${METAL_XTRANS_INTERPOLATE_METALLIB}"
        ALCEDO_METAL_CVT_REF_SPACE_METALLIB_PATH="${METAL_CVT_REF_SPACE_METALLIB}"
        ALCEDO_METAL_GEOMETRY_UTILS_METALLIB_PATH="${METAL_GEOMETRY_UTILS_METALLIB}"
    )
endif()
if(ALCEDO_OPENCL_ENABLED)
    target_link_libraries(RawProcessorOp PUBLIC OpenClProgramLibrary)
    target_compile_definitions(RawProcessorOp PUBLIC HAVE_OPENCL)
endif()

# --- Accelerator Backend Selection ---
def_library(AcceleratorBackend
    SOURCES ${ALCEDO_SRC_ROOT}/edit/pipeline/pipeline_accelerator.cpp
    PUBLIC_DEPS Image
    PRIVATE_DEPS ${ALCEDO_OPENCV_CORE_DEPS} CudaDriverRequirements
)
if(ALCEDO_CUDA_ENABLED)
    target_compile_definitions(AcceleratorBackend PUBLIC HAVE_CUDA)
    target_include_directories(AcceleratorBackend PRIVATE ${CUDAToolkit_INCLUDE_DIRS})
endif()
if(ALCEDO_METAL_ENABLED)
    target_compile_definitions(AcceleratorBackend PUBLIC HAVE_METAL)

endif()
if(ALCEDO_OPENCL_ENABLED)
    target_compile_definitions(AcceleratorBackend PUBLIC HAVE_OPENCL)
    target_link_libraries(AcceleratorBackend PRIVATE OpenClProgramLibrary)
endif()

# --- Edit Pipeline ---
# The executor binds a PipelineDocument and dispatches to the GPU DAG renderer of the resolved
# backend. The legacy stage pipeline is archived (G10.9).
def_cuda_library(EditPipeline
    SOURCES ${ALCEDO_SRC_ROOT}/edit/pipeline/pipeline_executor.cpp
    PUBLIC_DEPS ImageBuffer EditGraph EditLutResources
    PRIVATE_DEPS ThreadPool CudaDriverRequirements AcceleratorBackend
)

if(ALCEDO_CUDA_ENABLED)
    set_target_properties(EditPipeline PROPERTIES CUDA_SEPARABLE_COMPILATION ON)
    set_target_properties(EditPipeline PROPERTIES CUDA_RESOLVE_DEVICE_SYMBOLS ON)
    target_compile_definitions(EditPipeline PUBLIC HAVE_CUDA)
    target_include_directories(EditPipeline PRIVATE ${CUDAToolkit_INCLUDE_DIRS})
    target_link_libraries(EditPipeline PRIVATE CUDA::cudart)
endif()
if(ALCEDO_METAL_ENABLED)
    target_compile_definitions(EditPipeline PUBLIC HAVE_METAL)
    target_link_libraries(EditPipeline PRIVATE MetalImage MetalContext)
endif()
if(ALCEDO_OPENCL_ENABLED)
    target_compile_definitions(EditPipeline PUBLIC HAVE_OPENCL)
    target_link_libraries(EditPipeline PRIVATE OpenClProgramLibrary EditRuntimeOpenCl)
endif()

set(RENDERER_SRCS ${ALCEDO_SRC_ROOT}/renderer/pipeline_scheduler.cpp)
def_library(PipelineScheduler
    SOURCES ${RENDERER_SRCS}
    PUBLIC_DEPS EditPipeline ThreadPool ImageLoader
    PRIVATE_DEPS PreviewPerformance)

# Hard-coded DemosaicNet modules + lazy model cache. Owned by the CUDA Neural Engine develop path
# (cuda_demosaicnet.cu), NOT by RawProcessorOp.
if(ALCEDO_CUDA_ENABLED)
    def_cuda_library(DemosaicNet
        SOURCES
            ${ALCEDO_SRC_ROOT}/decoders/processor/nn/demosaicnet_bayer.cu
            ${ALCEDO_SRC_ROOT}/decoders/processor/nn/demosaicnet_xtrans.cu
            ${ALCEDO_SRC_ROOT}/decoders/processor/nn/demosaicnet_cache.cpp
            ${ALCEDO_SRC_ROOT}/decoders/processor/nn/demosaicnet_profiler.cpp
        PUBLIC_DEPS
            CudaUtils
    )
    set_target_properties(DemosaicNet PROPERTIES CUDA_SEPARABLE_COMPILATION ON)
    target_compile_definitions(DemosaicNet PUBLIC HAVE_CUDA)
    target_compile_definitions(DemosaicNet PRIVATE
        ALCEDO_DEMOASICNET_MODEL_DIR="${ALCEDO_SRC_ROOT}/config/models"
    )

    # Product-path Neural preprocess (phase-align + gamma) + GpuMat demosaic entry.
    # Stays on CudaDemosaicNetEntry (not generic DemosaicNet modules) so LibRaw CFA types link.
    def_cuda_library(CudaDemosaicNetEntry
        SOURCES
            ${ALCEDO_SRC_ROOT}/decoders/processor/operators/gpu/cuda_demosaicnet.cu
            ${ALCEDO_SRC_ROOT}/decoders/processor/nn/demosaicnet_preprocess.cu
        PUBLIC_DEPS
            DemosaicNet
            libraw::raw_r
    )
    set_target_properties(CudaDemosaicNetEntry PROPERTIES CUDA_SEPARABLE_COMPILATION ON)
    target_compile_definitions(CudaDemosaicNetEntry PUBLIC HAVE_CUDA)
endif()

# AI Denoise: the overlap tile planner, the tile loop, and the tile input and output of the UtNet2
# sessions (all platforms; no ONNX Runtime type).
def_library(UtNet2TileLoop
    SOURCES
        ${ALCEDO_SRC_ROOT}/decoders/processor/nn/overlap_tile_jobs.cpp
        ${ALCEDO_SRC_ROOT}/decoders/processor/nn/utnet2_tile_loop.cpp
        ${ALCEDO_SRC_ROOT}/decoders/processor/nn/utnet2_tile_io.cpp
    PRIVATE_DEPS opencv_core
)

# AI Denoise: the ONNX Runtime session of the FP16 UtNet2 graph on DirectML (Windows x64, for each
# render backend). Only this target includes ONNX Runtime headers; its public header has no ONNX
# Runtime type. onnxruntime.dll is delay-loaded, so the executable that links this target starts
# without it and the session reports the load error. The session creates its own Direct3D 12
# device and queue for DirectML (d3d12, dxgi); DirectML.dll is loaded at run time. pdh reads the
# video memory that all processes use on the adapter (issue #1043).
include("${CMAKE_SOURCE_DIR}/scripts/cmake/fetch_onnxruntime_directml.cmake")
if(TARGET alcedo::onnxruntime_directml)
    # The Windows libraries (d3d12, dxgi, pdh, delayimp) are named in the source with
    # `#pragma comment(lib)`: a CUDA library that links this target through EditInput passes its
    # link libraries to the device link of nvcc, which cannot find a library by its bare name.
    def_library(UtNet2DirectMl
        SOURCES
            ${ALCEDO_SRC_ROOT}/decoders/processor/nn/utnet2_directml_session.cpp
            ${ALCEDO_SRC_ROOT}/decoders/processor/nn/utnet2_onnx_graph.cpp
        PUBLIC_DEPS UtNet2TileLoop
        PRIVATE_DEPS alcedo::onnxruntime_directml
    )
    # The delay load applies to the host link of the executable, so it is part of the interface.
    target_link_options(UtNet2DirectMl INTERFACE "$<HOST_LINK:/DELAYLOAD:onnxruntime.dll>")
endif()

# AI Denoise: the ONNX Runtime sessions of the FP16 UtNet2 graph on the Neural Engine (macOS on
# Apple silicon, DN10). Only this target includes ONNX Runtime headers; its public header has no
# ONNX Runtime type. libonnxruntime.1.24.4.dylib is linked: the build tree finds it through the
# build rpath, the bundle in Contents/Frameworks (CMakeLists.txt of the repository).
include("${CMAKE_SOURCE_DIR}/scripts/cmake/fetch_onnxruntime_macos.cmake")
if(TARGET alcedo::onnxruntime_macos)
    def_library(UtNet2CoreMl
        SOURCES
            ${ALCEDO_SRC_ROOT}/decoders/processor/nn/utnet2_coreml_session.cpp
            ${ALCEDO_SRC_ROOT}/decoders/processor/nn/utnet2_onnx_graph.cpp
        PUBLIC_DEPS UtNet2TileLoop
        PRIVATE_DEPS alcedo::onnxruntime_macos
    )
    target_compile_definitions(UtNet2CoreMl PRIVATE
        ALCEDO_ONNXRUNTIME_MACOS_VERSION="${ALCEDO_ONNXRUNTIME_MACOS_VERSION}")
endif()


# ==============================================================================
# 4. Decoding & IO
# ==============================================================================

set(DECODER_SRCS
    ${ALCEDO_SRC_ROOT}/decoders/decoder_scheduler.cpp
    ${ALCEDO_SRC_ROOT}/decoders/thumbnail_decoder.cpp
    ${ALCEDO_SRC_ROOT}/decoders/metadata_decoder.cpp
)

def_library(ImageDecoder
    SOURCES ${DECODER_SRCS}
    PUBLIC_DEPS Image ThreadPool libraw::raw_r Exiv2
    PRIVATE_DEPS ${ALCEDO_OPENCV_IMAGE_DEPS}
)

def_library(ImageLoader 
    SOURCES ${ALCEDO_SRC_ROOT}/io/image/image_loader.cpp
    PUBLIC_DEPS ImageDecoder
)

# ==============================================================================
