# alcedo-cli: console client for automation sessions. It links only the automation protocol and
# Qt, never the album backend, so that it starts fast and stays small.

add_executable(alcedo_cli
    "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/cli_commands.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/cli_commands.hpp"
)
target_link_libraries(alcedo_cli PRIVATE AutomationProtocol Qt6::Core Qt6::Network)
# `alcedo-cli --version` prints the Alcedo Studio version of the package.
target_compile_definitions(alcedo_cli PRIVATE ALCEDO_APP_VERSION="${PROJECT_VERSION}")
# Keep the client beside alcedo_main, where `session start` looks for the host program.
set_target_properties(alcedo_cli PROPERTIES
    OUTPUT_NAME "alcedo-cli"
    RUNTIME_OUTPUT_DIRECTORY "${ALCEDO_BINARY_ROOT}"
)
if(APPLE)
    # The macOS package installs alcedo-cli in <Bundle>.app/Contents/MacOS, beside the bundle
    # executable, and the Qt frameworks in Contents/Frameworks.
    set_property(TARGET alcedo_cli APPEND PROPERTY
        INSTALL_RPATH "@executable_path/../Frameworks")
    if(ALCEDO_MACOS_BUNDLE)
        # The root CMakeLists.txt names the bundle executable after the bundle.
        target_compile_definitions(alcedo_cli PRIVATE
            ALCEDO_MACOS_BUNDLE_EXECUTABLE="${ALCEDO_MACOS_BUNDLE_NAME}")
    endif()
endif()
alcedo_copy_linked_runtime_dlls(alcedo_cli)
