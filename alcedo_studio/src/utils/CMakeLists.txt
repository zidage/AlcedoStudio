# ==============================================================================
# Utils targets extracted from alcedo_studio/src/CMakeLists.txt (Phase 3D).
# ==============================================================================

def_library(TimeProvider
    SOURCES ${ALCEDO_SRC_ROOT}/utils/clock/time_provider.cpp
)

if(WIN32)
    def_library(PreviewPerformance TYPE SHARED
        SOURCES
            ${ALCEDO_SRC_ROOT}/utils/diagnostics/preview_performance.cpp
            ${ALCEDO_SRC_ROOT}/include/utils/diagnostics/preview_performance.hpp
            ${ALCEDO_SRC_ROOT}/include/utils/diagnostics/preview_performance_record.hpp
    )
    set_target_properties(PreviewPerformance PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
    target_compile_options(PreviewPerformance PRIVATE
        $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>,$<CONFIG:MinSizeRel>>>:/GL->
    )
    install(TARGETS PreviewPerformance RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
else()
    def_library(PreviewPerformance
        SOURCES
            ${ALCEDO_SRC_ROOT}/utils/diagnostics/preview_performance.cpp
            ${ALCEDO_SRC_ROOT}/include/utils/diagnostics/preview_performance.hpp
            ${ALCEDO_SRC_ROOT}/include/utils/diagnostics/preview_performance_record.hpp
    )
endif()

def_library(AppDiagnostics
    SOURCES
        ${ALCEDO_SRC_ROOT}/utils/diagnostics/app_logging.cpp
        ${ALCEDO_SRC_ROOT}/include/utils/diagnostics/app_logging.hpp
        ${ALCEDO_SRC_ROOT}/utils/diagnostics/render_e2e_timing.cpp
        ${ALCEDO_SRC_ROOT}/include/utils/diagnostics/render_e2e_timing.hpp
    PUBLIC_DEPS Qt6::Core PreviewPerformance
)

def_library(CudaDriverRequirements
    SOURCES ${ALCEDO_SRC_ROOT}/utils/cuda/cuda_driver_requirements.cpp
)

def_library(StrConv
    SOURCES ${ALCEDO_SRC_ROOT}/utils/string/converter.cpp
    PUBLIC_DEPS utfcpp
)
