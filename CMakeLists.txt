# LonelyIce: single-player launcher around the LonelyIce fork of AzerothCore.
#
#   git clone --recursive https://github.com/LonelyIceProject/lonelyice
#   tools\build-ui-deps.ps1                      (SDL3, FreeType, RmlUi into deps/)
#   cmake -S . -B build -DBOOST_ROOT=... -DOPENSSL_ROOT_DIR=...
#   cmake --build build --config RelWithDebInfo --target lonelyice_release
#
# The core is built as shared libraries with the plugin loader; plugins (LONELYICE_PLUGIN_DIRS) are built next to it.
cmake_minimum_required(VERSION 3.22)
project(LonelyIce LANGUAGES CXX C)

set(LONELYICE_CORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/azerothcore" CACHE PATH
  "Checkout of LonelyIceProject/azerothcore-wotlk")
# Plugins shipped with LonelyIce: every plugins/<name> with a plugin.json (git submodules), unless given explicitly.
file(GLOB LONELYICE_DEFAULT_PLUGINS LIST_DIRECTORIES true "${CMAKE_CURRENT_SOURCE_DIR}/plugins/*")
set(LONELYICE_FOUND_PLUGINS "")
foreach(dir ${LONELYICE_DEFAULT_PLUGINS})
  if (EXISTS "${dir}/plugin.json" AND EXISTS "${dir}/CMakeLists.txt")
    list(APPEND LONELYICE_FOUND_PLUGINS "${dir}")
  endif()
endforeach()
set(LONELYICE_PLUGIN_DIRS "${LONELYICE_FOUND_PLUGINS}" CACHE STRING
  "Plugin sources to build and ship with LonelyIce (;-separated); default: plugins/*")

if (NOT EXISTS "${LONELYICE_CORE_DIR}/CMakeLists.txt")
  message(FATAL_ERROR "AzerothCore not found in ${LONELYICE_CORE_DIR}: run `git submodule update --init` or set LONELYICE_CORE_DIR")
endif()

# What LonelyIce needs from the core.
set(WITH_DYNAMIC_LINKING ON CACHE BOOL "Core as shared libraries (plugins link against them)" FORCE)
set(WITH_PLAYERBOTS_HOOKS ON CACHE BOOL "Core hooks for the playerbots plugin" FORCE)
set(AC_PLUGIN_ABI "lonelyice-ac-1" CACHE STRING "Plugin binary interface of LonelyIce releases" FORCE)
set(AC_PLUGIN_SOURCE_DIRS "${LONELYICE_PLUGIN_DIRS}" CACHE STRING "" FORCE)
set(WITH_MYSQL OFF CACHE BOOL "LonelyIce runs on SQLite only; no MySQL client library" FORCE)
set(TOOLS_BUILD "maps-only" CACHE STRING "LonelyIce embeds the map extractors" FORCE)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

add_subdirectory("${LONELYICE_CORE_DIR}" azerothcore)
add_subdirectory(src)
