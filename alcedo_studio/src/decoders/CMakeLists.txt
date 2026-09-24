# Decoders target ownership (RAW processor, DemosaicNet, image decoding).
# 3. Processors & Operators
# ==============================================================================

# --- RawProcessorOp Configuration (CPU/GPU Split) ---
set(RAW_OP_CPU_SRCS
    ${ALCEDO_SRC_ROOT}/decoders/processor/operators/cpu/color_space_conv.cpp
    ${ALCEDO_SRC_ROOT}/decoders/processor/operators/cpu/debayer_ahd.cpp
    ${ALCEDO_SRC_ROOT}/decoders/processor/operators/cpu/debayer_amaze.cpp
    ${ALCEDO_SRC_ROOT}/decoders/processor/operators/cpu/debayer_rcd.cpp
    ${ALCEDO_SRC_ROOT}/decoders/processor/operators/cpu/highlight_reconstruct.cpp
    ${ALCEDO_SRC_ROOT}/decoders/processor/operators/cpu/white_balance.cpp
    ${ALCEDO_SRC_ROOT}/decoders/processor/operators/cpu/raw_proc_utils.cpp
)

set(RAW_OP_GPU_SRCS)
if(ALCEDO_CUDA_ENABLED)
    list(APPEND RAW_OP_GPU_SRCS
        ${ALCEDO_SRC_ROOT}/decoders/processor/operators/gpu/cuda_color_space_conv.cu
        ${ALCEDO_SRC_ROOT}/decoders/processor/operators/gpu/cuda_debayer_rcd.cu
        ${ALCEDO_SRC_ROOT}/decoders/processor/operators/gpu/cuda_dng_warp.cu
        ${ALCEDO_SRC_ROOT}/decoders/processor/operators/gpu/cuda_downsample.cu
        ${ALCEDO_SRC_ROOT}/decoders/processor/operators/gpu/cuda_highlight_reconstruct.cu
        ${ALCEDO_SRC_ROOT}/decoders/processor/operators/gpu/cuda_image_ops.cu
        ${ALCEDO_SRC_ROOT}/decoders/processor/operators/gpu/cuda_rotate.cu
        ${ALCEDO_SRC_ROOT}/decoders/processor/operators/gpu/cuda_white_balance.cu
        ${ALCEDO_SRC_ROOT}/decoders/processor/operators/gpu/cuda_xtrans_interpolate.cu
    )
endif()
if(ALCEDO_METAL_ENABLED)
    list(APPEND RAW_OP_GPU_SRCS
        ${ALCEDO_SRC_ROOT}/decoders/processor/operators/gpu/metal_cvt_ref_space.cpp
        ${ALCEDO_SRC_ROOT}/decoders/processor/operators/gpu/metal_debayer_rcd.cpp
        ${ALCEDO_SRC_ROOT}/decoders/processor/operators/gpu/metal_encode.cpp
        ${ALCEDO_SRC_ROOT}/decoders/processor/operators/gpu/metal_highlight_reconstruct.cpp
        ${ALCEDO_SRC_ROOT}/decoders/processor/operators/gpu/metal_to_linear_ref.cpp
        ${ALCEDO_SRC_ROOT}/decoders/processor/operators/gpu/metal_xtrans_interpolate.cpp
    )
endif()
if(ALCEDO_OPENCL_ENABLED)
    list(APPEND RAW_OP_GPU_SRCS
        ${ALCEDO_SRC_ROOT}/decoders/processor/operators/gpu/opencl_to_linear_ref.cpp
        ${ALCEDO_SRC_ROOT}/decoders/processor/operators/gpu/opencl_debayer_rcd.cpp
        ${ALCEDO_SRC_ROOT}/decoders/processor/operators/gpu/opencl_xtrans_interpolate.cpp
        ${ALCEDO_SRC_ROOT}/decoders/processor/operators/gpu/opencl_highlight_reconstruct.cpp
        ${ALCEDO_SRC_ROOT}/decoders/processor/operators/gpu/opencl_cvt_ref_space.cpp
        ${ALCEDO_SRC_ROOT}/decoders/processor/operators/gpu/opencl_encode.cpp
    )
endif()

# RawProcessorOp Target
def_cuda_library(RawProcessorOp
    SOURCES ${RAW_OP_CPU_SRCS} ${RAW_OP_GPU_SRCS}
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
    PUBLIC_DEPS ImageBuffer EditGraph
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

# --- Raw Processor Wrapper ---
set(RAW_PROCESSOR_BACKEND_SRCS)
if(ALCEDO_CUDA_ENABLED)
    list(APPEND RAW_PROCESSOR_BACKEND_SRCS ${ALCEDO_SRC_ROOT}/decoders/processor/raw_processor_cuda.cpp)
endif()
if(ALCEDO_METAL_ENABLED)
    list(APPEND RAW_PROCESSOR_BACKEND_SRCS ${ALCEDO_SRC_ROOT}/decoders/processor/raw_processor_metal.cpp)
endif()
if(ALCEDO_OPENCL_ENABLED)
    list(APPEND RAW_PROCESSOR_BACKEND_SRCS ${ALCEDO_SRC_ROOT}/decoders/processor/raw_processor_opencl.cpp)
endif()

# Hard-coded DemosaicNet modules + lazy model cache. Owned by the RAW CUDA backend path
# (raw_processor_cuda.cpp / cuda_demosaicnet.cu), NOT by RawProcessorOp (legacy op facade only).
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

def_library(RawProcessor
    SOURCES ${ALCEDO_SRC_ROOT}/decoders/processor/raw_processor.cpp ${RAW_PROCESSOR_BACKEND_SRCS}
    PUBLIC_DEPS Image libraw::raw_r Exiv2 RawProcessorOp
    PRIVATE_DEPS opencv_imgproc AcceleratorBackend
)
if(ALCEDO_CUDA_ENABLED)
    target_link_libraries(RawProcessor PRIVATE CudaDemosaicNetEntry)
endif()
if(ALCEDO_METAL_ENABLED)
    target_link_libraries(RawProcessor PRIVATE MetalDemosaicNetEntry MetalGeometryUtils)
endif()
if(ALCEDO_OPENCL_ENABLED)
    target_link_libraries(RawProcessor PRIVATE OpenClGeometryUtils OpenClDemosaicNetEntry)
endif()

# ==============================================================================
# 4. Decoding & IO
# ==============================================================================

set(DECODER_SRCS
    ${ALCEDO_SRC_ROOT}/decoders/decoder_scheduler.cpp
    ${ALCEDO_SRC_ROOT}/decoders/raw_decoder.cpp
    ${ALCEDO_SRC_ROOT}/decoders/thumbnail_decoder.cpp
    ${ALCEDO_SRC_ROOT}/decoders/metadata_decoder.cpp
)

def_library(ImageDecoder
    SOURCES ${DECODER_SRCS}
    PUBLIC_DEPS Image ThreadPool libraw::raw_r RawProcessor
    PRIVATE_DEPS ${ALCEDO_OPENCV_IMAGE_DEPS}
)

def_library(ImageLoader 
    SOURCES ${ALCEDO_SRC_ROOT}/io/image/image_loader.cpp
    PUBLIC_DEPS ImageDecoder
)

# ==============================================================================
