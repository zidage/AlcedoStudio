set(VCPKG_POLICY_EMPTY_PACKAGE enabled)
set(VCPKG_POLICY_EMPTY_INCLUDE_FOLDER enabled)
file(MAKE_DIRECTORY "${CURRENT_PACKAGES_DIR}/share/${PORT}")
file(WRITE "${CURRENT_PACKAGES_DIR}/share/${PORT}/copyright"
  "Alcedo Studio builds Lensfun from alcedo_studio/src/third_party/lensfun.\n")
message(STATUS
  "vcpkg port lensfun is disabled; Alcedo uses the lensfun git submodule.")
