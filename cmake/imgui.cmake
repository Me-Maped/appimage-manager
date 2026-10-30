# Dear ImGui sources.
#
# The fork this project is built on (https://github.com/Me-Maped/imgui) is a
# git submodule at third_party/imgui, so upstream theme/backends updates are a
# `git submodule update --remote` away.
#
# Resolution order:
#   1. -DAIMGR_IMGUI_DIR=/path/to/imgui   (explicit override)
#   2. third_party/imgui                  (submodule, preferred)
#   3. FetchContent from the fork         (only when the directory is absent)
set(IMGUI_DIR "" CACHE PATH "Local dear imgui source tree (overrides the submodule)")
option(AIMGR_IMGUI_FETCH "Fetch imgui when the submodule directory is absent" ON)

get_filename_component(_aimgr_project_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(_aimgr_submodule "${_aimgr_project_root}/third_party/imgui")

if(NOT IMGUI_DIR)
  if(EXISTS "${_aimgr_submodule}/imgui.h")
    set(IMGUI_DIR "${_aimgr_submodule}")
  elseif(EXISTS "${_aimgr_project_root}/.gitmodules" AND EXISTS "${_aimgr_submodule}")
    # Declared as a submodule but not checked out: never silently substitute a
    # different source, tell the user exactly what to run.
    message(FATAL_ERROR
      "third_party/imgui is an uninitialised git submodule.\n"
      "Run:  git submodule update --init --recursive\n"
      "(or configure with -DAIMGR_IMGUI_DIR=/path/to/imgui)")
  elseif(AIMGR_IMGUI_FETCH)
    include(FetchContent)
    message(STATUS "imgui submodule missing, fetching https://github.com/Me-Maped/imgui.git")
    FetchContent_Declare(imgui
      GIT_REPOSITORY https://github.com/Me-Maped/imgui.git
      GIT_TAG        master
      GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(imgui)
    set(IMGUI_DIR "${imgui_SOURCE_DIR}")
  else()
    message(FATAL_ERROR
      "Dear ImGui not found. Run `git submodule update --init --recursive`, "
      "or pass -DAIMGR_IMGUI_DIR=/path/to/imgui, or -DAIMGR_IMGUI_FETCH=ON.")
  endif()
endif()

if(NOT EXISTS "${IMGUI_DIR}/imgui.cpp")
  message(FATAL_ERROR "imgui sources not found in IMGUI_DIR='${IMGUI_DIR}'")
endif()

set(IMGUI_SOURCES
  ${IMGUI_DIR}/imgui.cpp
  ${IMGUI_DIR}/imgui_draw.cpp
  ${IMGUI_DIR}/imgui_tables.cpp
  ${IMGUI_DIR}/imgui_widgets.cpp
  ${IMGUI_DIR}/backends/imgui_impl_sdl2.cpp
  ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp)

# The fork ships an application-side theme module (misc/themes). It is optional:
# if upstream ever moves it, the GUI simply falls back to native ImGui styling.
set(IMGUI_HAS_THEMES FALSE)
if(EXISTS "${IMGUI_DIR}/misc/themes/imgui_themes.cpp")
  list(APPEND IMGUI_SOURCES "${IMGUI_DIR}/misc/themes/imgui_themes.cpp")
  set(IMGUI_HAS_THEMES TRUE)
endif()

message(STATUS "imgui:  ${IMGUI_DIR}")
message(STATUS "themes: ${IMGUI_HAS_THEMES}")
