# Concurrency target ownership.
set(ALCEDO_CONCURRENCY_ROOT "${ALCEDO_SRC_ROOT}/concurrency")
def_library(ThreadPool SOURCES "${ALCEDO_SRC_ROOT}/concurrency/thread_pool.cpp")
if(ALCEDO_METAL_ENABLED)
    target_link_libraries(ThreadPool PUBLIC alcedo::metal_cpp)
endif()
