# ==============================================================================
# Utils targets extracted from alcedo_studio/src/CMakeLists.txt (Phase 3D).
# ==============================================================================

def_library(TimeProvider
    SOURCES ${ALCEDO_SRC_ROOT}/utils/clock/time_provider.cpp
)

def_library(AppDiagnostics
    SOURCES
        ${ALCEDO_SRC_ROOT}/utils/diagnostics/app_logging.cpp
        ${ALCEDO_SRC_ROOT}/include/utils/diagnostics/app_logging.hpp
    PUBLIC_DEPS Qt6::Core
)

def_library(CudaDriverRequirements
    SOURCES ${ALCEDO_SRC_ROOT}/utils/cuda/cuda_driver_requirements.cpp
)

def_library(StrConv
    SOURCES ${ALCEDO_SRC_ROOT}/utils/string/converter.cpp
    PUBLIC_DEPS utfcpp
)
