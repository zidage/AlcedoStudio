# alcedo-cli: console client for automation sessions. It links only the automation protocol and
# Qt, never the album backend, so that it starts fast and stays small.

add_executable(alcedo_cli
    "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/cli_commands.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/cli_commands.hpp"
)
target_link_libraries(alcedo_cli PRIVATE AutomationProtocol Qt6::Core Qt6::Network)
# Keep the client beside alcedo_main, where `session start` looks for the host program.
set_target_properties(alcedo_cli PROPERTIES
    OUTPUT_NAME "alcedo-cli"
    RUNTIME_OUTPUT_DIRECTORY "${ALCEDO_BINARY_ROOT}"
)
alcedo_copy_linked_runtime_dlls(alcedo_cli)
