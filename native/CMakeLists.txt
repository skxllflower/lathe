# Lathe's native GUI (vpf): the converter window, standalone, replacing the Tauri GUI in gui/. Its own build, outside
# the core's (the lathe CLI at the repo root stays the worker the window drives).
#
#   cmake -S native -B native/build -A x64 [-DFETCHCONTENT_SOURCE_DIR_VACANTLABS=<vacant-labs checkout>]
#                                           [-DFETCHCONTENT_SOURCE_DIR_WAVDESK=<wavdesk checkout>]
#   cmake --build native/build --config Release     -> native/build/bin/Release/lathe-gui.exe
#   macOS: cmake -S native -B native/build -DCMAKE_BUILD_TYPE=Release && cmake --build native/build
#                                                    -> native/build/bin/Lathe.app (build/lathe, the core, copied in)
#   a release: tools/build-release-native.ps1 (Windows), tools/build-release-native-mac.sh (macOS)
#
# The window itself is WAVdesk's native Lathe window (native/shell/lathe_window.*, on the shared ExtWindow, the skins, the
# dialogs and the tool runner), compiled from a pinned WAVdesk commit rather than forked: the two apps show one window.
# app/ is what the standalone adds around it (its own window frame, title buttons, skin switch, About, the launch
# arguments, the self-registration) and app/host_stubs.cpp the few WAVdesk-library calls those shared files make.
# vpf and WAVdesk are both pinned to commits (never a branch); for work on either alongside this, point
# FETCHCONTENT_SOURCE_DIR_<NAME> at a local checkout.
cmake_minimum_required(VERSION 3.20)
if(CMAKE_HOST_APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Minimum macOS (the core's floor)" FORCE)
endif()
project(lathe_native C CXX)
if(APPLE)
    enable_language(OBJCXX)   # (the shell's macOS side: os_shell_mac.mm)
endif()
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# the version: tauri.conf.json's, Lathe's one source (cmake/lathe_version.cmake: the core's CLI reads it too)
include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/lathe_version.cmake)

include(FetchContent)
FetchContent_Declare(vacantlabs
    GIT_REPOSITORY https://github.com/skxllflower/vacant-labs.git
    GIT_TAG        b6c228b23e852b20850f9d40d321f1aa81aff59c   # vacant-labs main (Etch 0.1.3, the manager 0.3.3: the 2026-11-06 round; VX Native's middle dot and en dash); vacant-labs main (Etch 0.1.2, the manager 0.3.2: the 2026-11-05 round); vacant-labs main (the 0.1.1 release: the drag-out race, the plugin host helper); vacant-labs main (the Etch bug sweep, the vpf menu fix, the manager's Mac port); (WAVdesk native's own pin: the shared window was written on it)
    GIT_SUBMODULES vpf/third_party/clap vpf/third_party/vst3/pluginterfaces
)
FetchContent_Declare(wavdesk
    GIT_REPOSITORY https://github.com/skxllflower/wavdesk.git
    GIT_TAG        1bb3f1917a181205bc34fd5ccb4b1b8c8ac46cfc   # wavdesk main (0.3.3: the Latch chop audit, its vacant-labs pin b6c228b2); wavdesk main (0.3.2: the Latch chop audit fixes, the tree paste, its vacant-labs pin af95bf99); wavdesk main (0.3.1, its vacant-labs pin a2990c99); wavdesk main (ExtWindow::macButtons, the scrolled rules cut to their column); wavdesk main (its vacant-labs pin 7895084e); wavdesk main (the Mac shell merged in) (macOS: a standalone bundle's own coredist core, never the GUI itself), on feat/native-shell (--setup's verbs under HKCU, not the HKLM root; the Manager --setup / --uninstall; the Latch hooks, Lathe parity, folder groups; 1-Bit; VX menu hover; vacant-labs 030d8d34)
    GIT_SUBMODULES ""
)
set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install    OFF CACHE INTERNAL "")
FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)
foreach(dep vacantlabs wavdesk)
    FetchContent_GetProperties(${dep})
    if(NOT ${dep}_POPULATED)
        FetchContent_Populate(${dep})
    endif()
endforeach()
FetchContent_MakeAvailable(nlohmann_json)
set(VPF_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(${vacantlabs_SOURCE_DIR}/vpf ${CMAKE_BINARY_DIR}/vpf)

# WAVdesk's shell files the window is made of (each one unchanged; native/shell in the pinned commit)
set(WD ${wavdesk_SOURCE_DIR}/native/shell)
set(LATHE_SHARED_SOURCES
    ${WD}/lathe_window.cpp ${WD}/ext_window.cpp ${WD}/external_tools.cpp
    ${WD}/look.cpp ${WD}/skin.cpp ${WD}/skin_focused.cpp ${WD}/skin_vx.cpp ${WD}/skin_zen.cpp
    ${WD}/dialog_window.cpp ${WD}/popup_motion.cpp ${WD}/window_place.cpp ${WD}/window_fit.cpp
    ${WD}/ui_state.cpp ${WD}/settings_store.cpp ${WD}/core_process.cpp ${WD}/dev_run.cpp ${WD}/drag_chip.cpp
    ${WD}/menus.cpp ${WD}/context_menu.cpp)
set(LATHE_APP_SOURCES app/main.cpp app/lathe_app.cpp app/about_window.cpp app/settings_window.cpp app/host_stubs.cpp app/self_register.cpp app/tauri_import.cpp app/app_icon.cpp app/setup_cli.cpp)

if(APPLE)
    set(LATHE_EXE_NAME Lathe)        # (Lathe.app, as the Tauri bundle was named)
else()
    set(LATHE_EXE_NAME lathe-gui)    # (the Tauri exe name: the installer, the uninstall key and WAVdesk look for it)
endif()
vpf_add_standalone_app(lathe_gui NAME ${LATHE_EXE_NAME} BUNDLE_ID com.vacantsystems.lathe VERSION ${LATHE_VERSION}
    SOURCES ${LATHE_APP_SOURCES} ${LATHE_SHARED_SOURCES}
    PLIST ${CMAKE_CURRENT_SOURCE_DIR}/app/Info.plist.in
    ICON ${CMAKE_CURRENT_SOURCE_DIR}/../gui/src-tauri/icons/icon.icns
    WIN_RC ${CMAKE_CURRENT_BINARY_DIR}/lathe_gui.rc)
target_include_directories(lathe_gui PRIVATE ${WD} ${CMAKE_CURRENT_SOURCE_DIR}/compat)
target_link_libraries(lathe_gui PRIVATE vpf::app nlohmann_json::nlohmann_json)
target_compile_definitions(lathe_gui PRIVATE LATHE_VERSION="${LATHE_VERSION}")
# the VX skin's packs, embedded as WAVdesk embeds them (skin_vx.cpp reads kWavdeskVx)
file(GLOB LATHE_VX_FILES CONFIGURE_DEPENDS ${wavdesk_SOURCE_DIR}/native/assets/vx/*.json ${wavdesk_SOURCE_DIR}/native/assets/vx/*.pack)
vpf_embed_assets(lathe_gui SYMBOL kWavdeskVx FILES ${LATHE_VX_FILES})
# the app's own icon picture (the title strip, About: app_icon.cpp)
vpf_embed_assets(lathe_gui SYMBOL kLatheIcon FILES ${CMAKE_CURRENT_SOURCE_DIR}/../gui/src/assets/app-icon.png)
if(WIN32)
    set(LATHE_ICON ${CMAKE_CURRENT_SOURCE_DIR}/../gui/src-tauri/icons/icon.ico)
    configure_file(app/lathe_gui.rc.in ${CMAKE_CURRENT_BINARY_DIR}/lathe_gui.rc @ONLY)
    # the UTF-8 active code page: the CRT opens UTF-8 paths as such (the core binary's pair)
    target_sources(lathe_gui PRIVATE app/lathe_gui.manifest)
    target_compile_definitions(lathe_gui PRIVATE UNICODE _UNICODE)
    target_link_libraries(lathe_gui PRIVATE shell32 ole32)
elseif(APPLE)
    # the shell's macOS OS calls (context_menu.h's os::openPath and the rest: Finder, the window frames)
    target_sources(lathe_gui PRIVATE ${WD}/os_shell_mac.mm)
    set_source_files_properties(${WD}/os_shell_mac.mm PROPERTIES COMPILE_OPTIONS -fobjc-arc)
    target_link_libraries(lathe_gui PRIVATE "-framework Foundation" "-framework AppKit" "-framework CoreServices" "-framework IOKit")
    # the lathe CLI into the bundle after every build (app/bundle_core.cmake: Contents/MacOS/coredist/lathe, re-signed);
    # LATHE_CORE_BINARY is the core's own build (the repo's build/, as tools/build-release-native-mac.sh makes it; a
    # release passes its staged copy, the libav dylibs beside it)
    set(LATHE_CORE_BINARY ${CMAKE_CURRENT_SOURCE_DIR}/../build/lathe CACHE FILEPATH "The lathe CLI Lathe.app carries")
    add_custom_target(lathe_bundle_core ALL
        COMMAND ${CMAKE_COMMAND} -DCORE=${LATHE_CORE_BINARY} -DAPP=$<TARGET_BUNDLE_DIR:lathe_gui>
                -DIDENTITY=${VPF_CODESIGN_IDENTITY} -P ${CMAKE_CURRENT_SOURCE_DIR}/app/bundle_core.cmake
        COMMENT "Lathe.app: its lathe CLI" VERBATIM)
    add_dependencies(lathe_bundle_core lathe_gui)
endif()
