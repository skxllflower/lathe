# Lathe's one version: gui/src-tauri/tauri.conf.json's "version" (the file Vacant Systems Manager's pack reads too).
# Included by the core's CMakeLists.txt (before project(): the CLI's --version) and by native/CMakeLists.txt (the GUI's
# VERSIONINFO and About); the release scripts read the same file for the installer. A bump is that one line
# (tools/bump-version.ps1 also keeps gui/package.json and Cargo.toml in step); editing it reconfigures both builds.
#
# Sets LATHE_VERSION (x.y.z) and LATHE_VERSION_COMMAS (x,y,z,0).
get_filename_component(_lathe_conf "${CMAKE_CURRENT_LIST_DIR}/../gui/src-tauri/tauri.conf.json" ABSOLUTE)
file(READ "${_lathe_conf}" _lathe_conf_text)
string(REGEX MATCH "\"version\"[ \t\r\n]*:[ \t\r\n]*\"([0-9]+\\.[0-9]+\\.[0-9]+)\"" _lathe_version_match "${_lathe_conf_text}")
if(NOT CMAKE_MATCH_1)
    message(FATAL_ERROR "no x.y.z \"version\" in ${_lathe_conf}")
endif()
set(LATHE_VERSION "${CMAKE_MATCH_1}")
string(REPLACE "." "," LATHE_VERSION_COMMAS "${LATHE_VERSION},0")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_lathe_conf}")
