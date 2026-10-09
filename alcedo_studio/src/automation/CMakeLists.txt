# Automation protocol: JSON-RPC 2.0 messages over JSON Lines, error codes, and session files.
# Qt Core only, so that the alcedo-cli client links it without the application libraries.

set(ALCEDO_AUTOMATION_ROOT "${ALCEDO_SRC_ROOT}/automation")

def_library(AutomationProtocol
    SOURCES
        "${ALCEDO_AUTOMATION_ROOT}/automation_protocol.cpp"
        "${ALCEDO_INCLUDE_ROOT}/automation/automation_protocol.hpp"
    PUBLIC_DEPS
        Qt6::Core
)
