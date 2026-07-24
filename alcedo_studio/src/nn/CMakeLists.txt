# Host safetensors parser (backend-neutral; no CUDA / OpenCL dependency).
def_library(NnSafetensors
    SOURCES "${ALCEDO_SRC_ROOT}/nn/safetensors.cpp"
    PRIVATE_DEPS JSON
)
