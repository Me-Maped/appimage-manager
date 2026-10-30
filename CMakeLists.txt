cmake_minimum_required(VERSION 3.20)
project(aimgr VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(AIMGR_BUILD_GUI  "Build the ImGui desktop GUI (aimgr-gui)" ON)
option(AIMGR_BUILD_TESTS "Register the end-to-end test suite" ON)

include(GNUInstallDirs)

set(AIMGR_SELF_REPO "Me-Maped/appimage-manager" CACHE STRING
    "GitHub repo (owner/name) used by `aimgr self update`")

# Brand icon embedded into the binary so `aimgr self integrate` works even when
# the AppImage mount (or the install prefix) is gone.
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/packaging/aimgr.png AIMGR_ICON_HEX HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," AIMGR_ICON_BYTES "${AIMGR_ICON_HEX}")
string(LENGTH "${AIMGR_ICON_HEX}" AIMGR_ICON_HEXLEN)
math(EXPR AIMGR_ICON_LEN "${AIMGR_ICON_HEXLEN} / 2")
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/generated/aimgr_icon_png.h
"// generated from packaging/aimgr.png - do not edit
#pragma once
static const unsigned char kAimgrIconPng[] = {${AIMGR_ICON_BYTES}0x00};
static const unsigned long kAimgrIconPngLen = ${AIMGR_ICON_LEN}ul;
")

find_package(CURL REQUIRED)
find_package(Threads REQUIRED)

# ---------------------------------------------------------------- core library
add_library(aimgr_core STATIC
  src/util.cpp
  src/json.cpp
  src/sha256.cpp
  src/version.cpp
  src/http.cpp
  src/config.cpp
  src/source.cpp
  src/manager.cpp
  src/selfupdate.cpp
)
target_include_directories(aimgr_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
                                            ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(aimgr_core PUBLIC CURL::libcurl Threads::Threads)
target_compile_definitions(aimgr_core PUBLIC AIMGR_VERSION="${PROJECT_VERSION}"
                                                 AIMGR_SELF_REPO="${AIMGR_SELF_REPO}")
target_compile_options(aimgr_core PRIVATE -Wall -Wextra -Wno-unused-parameter)

# ------------------------------------------------------------------- CLI binary
add_executable(aimgr src/main.cpp)
target_link_libraries(aimgr PRIVATE aimgr_core)
target_compile_options(aimgr PRIVATE -Wall -Wextra)

# ---------------------------------------------------------------------- GUI app
if(AIMGR_BUILD_GUI)
  include(cmake/imgui.cmake)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
  find_package(OpenGL REQUIRED)

  add_executable(aimgr-gui gui/app.cpp ${IMGUI_SOURCES})
  target_include_directories(aimgr-gui PRIVATE ${IMGUI_DIR} ${IMGUI_DIR}/backends
                                               ${IMGUI_DIR}/misc/themes)
  if(IMGUI_HAS_THEMES)
    target_compile_definitions(aimgr-gui PRIVATE AIMGR_HAS_IMGUI_THEMES=1)
  endif()
  target_link_libraries(aimgr-gui PRIVATE aimgr_core PkgConfig::SDL2 OpenGL::GL ${CMAKE_DL_LIBS})
  target_compile_options(aimgr-gui PRIVATE -Wall -Wextra -Wno-unused-parameter)
else()
  message(STATUS "GUI disabled (AIMGR_BUILD_GUI=OFF)")
endif()

# ------------------------------------------------------------------------ tests
if(AIMGR_BUILD_TESTS)
  enable_testing()
  add_executable(aimgr-unit-tests tests/unit_tests.cpp)
  target_link_libraries(aimgr-unit-tests PRIVATE aimgr_core)
  add_test(NAME unit COMMAND aimgr-unit-tests)
  add_test(NAME e2e
           COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/e2e.sh $<TARGET_FILE:aimgr>
           WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
  set_tests_properties(e2e PROPERTIES TIMEOUT 300)

  if(AIMGR_BUILD_GUI)
    add_test(NAME gui-smoke
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/gui_smoke.sh $<TARGET_FILE:aimgr-gui>
             WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(gui-smoke PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77)
  endif()
endif()

# --------------------------------------------------------------------- install
install(TARGETS aimgr RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(AIMGR_BUILD_GUI)
  install(TARGETS aimgr-gui RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  install(FILES packaging/aimgr-gui.desktop
          DESTINATION ${CMAKE_INSTALL_DATADIR}/applications RENAME aimgr.desktop)
  install(FILES packaging/aimgr.png
          DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/256x256/apps)
endif()
