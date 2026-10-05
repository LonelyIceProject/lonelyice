# LonelyIce: launcher UI + auth + world in one executable (Windows, Linux, macOS).
# Deps (static SDL3, FreeType, RmlUi, StormLib, miniz, libcurl) are built by tools/build-ui-deps.ps1 (Windows) or
# tools/build-ui-deps.sh (Linux, macOS) into LONELYICE_DEPS.
# The core (LONELYICE_CORE_DIR) is added by the top-level CMakeLists.txt; its targets are linked here.

set(LONELYICE_DEPS "${CMAKE_SOURCE_DIR}/deps" CACHE PATH "Folder with sdl3/, freetype/, rmlui/ and src/rmlui (backends)")
set(LONELYICE_RMLUI_BACKENDS "${LONELYICE_DEPS}/src/rmlui/Backends")

list(APPEND CMAKE_PREFIX_PATH "${LONELYICE_DEPS}/sdl3" "${LONELYICE_DEPS}/freetype" "${LONELYICE_DEPS}/rmlui" "${LONELYICE_DEPS}/stormlib" "${LONELYICE_DEPS}/miniz" "${LONELYICE_DEPS}/curl")
if (LONELYICE_GUI)
  find_package(SDL3 CONFIG QUIET COMPONENTS SDL3-static)
  find_package(RmlUi CONFIG QUIET)
endif()
find_package(StormLib CONFIG QUIET)
find_package(miniz CONFIG QUIET)
# Static libcurl: its config package adds CURL_STATICLIB and the system libraries it needs.
find_package(CURL CONFIG QUIET)

if (NOT StormLib_FOUND OR NOT miniz_FOUND OR NOT CURL_FOUND)
  message(STATUS "LonelyIce: StormLib/miniz/libcurl not found in ${LONELYICE_DEPS}, skipping (run tools/build-ui-deps.ps1 or .sh)")
  return()
endif()
if (LONELYICE_GUI AND (NOT SDL3_FOUND OR NOT RmlUi_FOUND OR NOT EXISTS "${LONELYICE_RMLUI_BACKENDS}/RmlUi_Renderer_GL3.cpp"))
  message(FATAL_ERROR "LonelyIce: GUI dependencies missing. Build UI dependencies or set LONELYICE_GUI=OFF for a headless build.")
endif()

if (LONELYICE_TUI)
  find_package(ftxui 7 CONFIG QUIET)
  if (NOT ftxui_FOUND)
    include(FetchContent)
    # Scoped so FTXUI stays static without changing how the core or plugins are linked.
    function(lonelyice_fetch_ftxui)
      set(BUILD_SHARED_LIBS OFF)
      set(FTXUI_BUILD_DOCS OFF CACHE BOOL "" FORCE)
      set(FTXUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
      set(FTXUI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
      set(FTXUI_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
      FetchContent_Declare(ftxui
        GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI.git
        GIT_TAG f921fad208912747c17d129a8ef75ec7624b6eec) # v7.0.3
      FetchContent_MakeAvailable(ftxui)
    endfunction()
    lonelyice_fetch_ftxui()
  endif()
  if (MSVC)
    foreach(ftxui_target screen dom component)
      if (TARGET ${ftxui_target})
        get_target_property(ftxui_imported ${ftxui_target} IMPORTED)
        if (NOT ftxui_imported)
          target_compile_options(${ftxui_target} PRIVATE /FS)
        endif()
      endif()
    endforeach()
  endif()
endif()

set(AUTH_APP_DIR "${LONELYICE_CORE_DIR}/src/server/apps/authserver")
set(WORLD_APP_DIR "${LONELYICE_CORE_DIR}/src/server/apps/worldserver")

file(GLOB_RECURSE LONELYICE_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/*.h")

if (NOT LONELYICE_GUI)
  list(FILTER LONELYICE_SOURCES EXCLUDE REGEX "/launcher/(Launcher|StorageForm|Wizard|UiBackend|Tray)\\.(cpp|h)$")
endif()
if (NOT LONELYICE_TUI)
  list(FILTER LONELYICE_SOURCES EXCLUDE REGEX "/tui/")
endif()

set(TOOLS_DIR "${LONELYICE_CORE_DIR}/src/tools")
set(MMAPS_SOURCES
  "${TOOLS_DIR}/mmaps_generator/Config.cpp"
  "${TOOLS_DIR}/mmaps_generator/IntermediateValues.cpp"
  "${TOOLS_DIR}/mmaps_generator/MapBuilder.cpp"
  "${TOOLS_DIR}/mmaps_generator/TerrainBuilder.cpp")

list(APPEND LONELYICE_SOURCES
  "${AUTH_APP_DIR}/Server/AuthSession.cpp"
  "${AUTH_APP_DIR}/Authentication/AuthCodes.cpp"
  "${WORLD_APP_DIR}/ACSoap/ACSoap.cpp"
  ${MMAPS_SOURCES})
if (LONELYICE_GUI)
  list(APPEND LONELYICE_SOURCES
    "${LONELYICE_RMLUI_BACKENDS}/RmlUi_Platform_SDL.cpp"
    "${LONELYICE_RMLUI_BACKENDS}/RmlUi_Renderer_GL3.cpp")
endif()

# The extractor units include the tools' .cpp files; their own headers resolve relative to those files,
# map_extractor additionally includes "loadlib.h" from its loadlib/ folder.
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/tools/MapExtractorUnit.cpp" PROPERTIES
  INCLUDE_DIRECTORIES "${TOOLS_DIR};${TOOLS_DIR}/map_extractor/loadlib")
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/tools/VmapExtractorUnit.cpp" PROPERTIES
  INCLUDE_DIRECTORIES "${TOOLS_DIR}")

# Embedded assets: every file under assets/ becomes a byte array (EmbedAsset.cmake), looked up by relative path.
set(ASSET_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/assets")
file(GLOB_RECURSE ASSET_FILES CONFIGURE_DEPENDS "${ASSET_ROOT}/lang/*.lang")
if (LONELYICE_GUI)
  file(GLOB_RECURSE GUI_ASSET_FILES CONFIGURE_DEPENDS "${ASSET_ROOT}/ui/*" "${ASSET_ROOT}/fonts/*.ttf" "${ASSET_ROOT}/icons/*.rgba" "${ASSET_ROOT}/icons/*.tga")
  list(APPEND ASSET_FILES ${GUI_ASSET_FILES})
endif()
set(GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(ASSET_DECL "")
set(ASSET_INDEX "")
set(ASSET_NUM 0)
set(ASSET_SOURCES "")
foreach(ASSET ${ASSET_FILES} "${TOOLS_DIR}/mmaps_generator/mmaps-config.yaml")
  if (ASSET MATCHES "mmaps-config.yaml$")
    set(REL "tools/mmaps-config.yaml")
  else()
    file(RELATIVE_PATH REL "${ASSET_ROOT}" "${ASSET}")
  endif()
  set(SYMBOL "g_asset${ASSET_NUM}")
  set(OUT "${GEN_DIR}/assets/${SYMBOL}.cpp")
  add_custom_command(OUTPUT "${OUT}"
    COMMAND ${CMAKE_COMMAND} -DIN=${ASSET} -DOUT=${OUT} -DSYMBOL=${SYMBOL} -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedAsset.cmake"
    DEPENDS "${ASSET}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedAsset.cmake"
    VERBATIM)
  list(APPEND ASSET_SOURCES "${OUT}")
  string(APPEND ASSET_DECL "    extern unsigned char const ${SYMBOL}_data[];\n    extern std::size_t const ${SYMBOL}_size;\n")
  string(APPEND ASSET_INDEX "        { \"${REL}\", ${SYMBOL}_data, ${SYMBOL}_size },\n")
  math(EXPR ASSET_NUM "${ASSET_NUM} + 1")
endforeach()
file(CONFIGURE OUTPUT "${GEN_DIR}/AssetIndex.cpp" CONTENT
"#include <cstddef>
namespace LonelyIce
{
${ASSET_DECL}
    struct AssetEntry { char const* path; unsigned char const* data; std::size_t const& size; };
    extern AssetEntry const g_assetIndex[] = {
${ASSET_INDEX}    };
    extern std::size_t const g_assetCount = ${ASSET_NUM};
}
")
list(APPEND LONELYICE_SOURCES ${ASSET_SOURCES} "${GEN_DIR}/AssetIndex.cpp")
source_group("Generated\\Assets" FILES ${ASSET_SOURCES})
if (WIN32)
  # Icon 1 is what Explorer and the taskbar show for the exe.
  file(CONFIGURE OUTPUT "${GEN_DIR}/icon.rc" CONTENT "1 ICON \"${ASSET_ROOT}/icons/lonelyice-round.ico\"\n")
  list(APPEND LONELYICE_SOURCES "${GEN_DIR}/icon.rc")
  set_source_files_properties("${GEN_DIR}/icon.rc" PROPERTIES OBJECT_DEPENDS "${ASSET_ROOT}/icons/lonelyice-round.ico")
endif()

if (LONELYICE_GUI)
  add_executable(lonelyice WIN32 ${LONELYICE_SOURCES})
else()
  add_executable(lonelyice ${LONELYICE_SOURCES})
endif()
set_target_properties(lonelyice PROPERTIES OUTPUT_NAME "LonelyIce" FOLDER "server")
add_dependencies(lonelyice revision.h)

target_include_directories(lonelyice PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/common"
  "${CMAKE_CURRENT_SOURCE_DIR}/server"
  "${CMAKE_CURRENT_SOURCE_DIR}/launcher"
  "${CMAKE_CURRENT_SOURCE_DIR}/packages"
  "${CMAKE_CURRENT_SOURCE_DIR}/storage"
  "${CMAKE_CURRENT_SOURCE_DIR}/tools"
  "${CMAKE_CURRENT_SOURCE_DIR}/tui"
  "${AUTH_APP_DIR}/Server"
  "${AUTH_APP_DIR}/Authentication"
  "${WORLD_APP_DIR}/ACSoap"
  "${LONELYICE_RMLUI_BACKENDS}")

if (LONELYICE_GUI)
  target_compile_definitions(lonelyice PRIVATE LONELYICE_GUI RMLUI_SDL_VERSION_MAJOR=3)
  target_link_libraries(lonelyice PRIVATE RmlUi::RmlUi SDL3::SDL3-static)
endif()
if (LONELYICE_TUI)
  target_compile_definitions(lonelyice PRIVATE LONELYICE_TUI)
  target_link_libraries(lonelyice PRIVATE ftxui::component ftxui::dom ftxui::screen)
endif()

target_link_libraries(lonelyice
  PRIVATE
    acore-core-interface
    modules
    scripts
    game
    gsoap
    libsidecar
    shared
    sqlite3
    mpq
    zlib
    Recast
    fkYAML
    StormLib::storm
    miniz::miniz
    CURL::libcurl)

if (WIN32)
  target_link_libraries(lonelyice PRIVATE version psapi winmm shell32 ole32)
elseif (APPLE)
  target_link_libraries(lonelyice PRIVATE "-framework CoreFoundation" "-framework SystemConfiguration" "-framework Security")
else()
  target_link_libraries(lonelyice PRIVATE ${CMAKE_DL_LIBS} pthread)
endif()

if (MSVC AND LONELYICE_GUI)
  # WIN32 subsystem but keep the regular main(argc, argv) entry.
  set_property(TARGET lonelyice APPEND_STRING PROPERTY LINK_FLAGS " /ENTRY:mainCRTStartup")
elseif (NOT MSVC)
  # The core libraries and plugins sit next to the executable.
  if (APPLE)
    set_target_properties(lonelyice PROPERTIES BUILD_RPATH "@loader_path" INSTALL_RPATH "@loader_path")
  else()
    set_target_properties(lonelyice PROPERTIES BUILD_RPATH "$ORIGIN" INSTALL_RPATH "$ORIGIN")
  endif()
endif()

if (USE_REAL_LIBSIDECAR AND LIBSIDECAR_SHARED_FILE)
  add_custom_command(TARGET lonelyice POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${LIBSIDECAR_SHARED_FILE}" "$<TARGET_FILE_DIR:lonelyice>")
endif()

# OpenSSL's libraries and its legacy provider (the core requires legacy.dll next to the executable) so the build
# folder runs as is.
if (WIN32)
  get_filename_component(OPENSSL_RUNTIME_DIR "${OPENSSL_INCLUDE_DIR}/../bin" ABSOLUTE)
  foreach(lib libcrypto-3-x64.dll libssl-3-x64.dll legacy.dll)
    if (EXISTS "${OPENSSL_RUNTIME_DIR}/${lib}")
      add_custom_command(TARGET lonelyice POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${OPENSSL_RUNTIME_DIR}/${lib}" "$<TARGET_FILE_DIR:lonelyice>")
    endif()
  endforeach()
endif()

install(TARGETS lonelyice DESTINATION "${CMAKE_INSTALL_PREFIX}")

# Release folder: LonelyIce, the core libraries and plain worldserver/authserver, the plugins, the libraries they need
# (taken from the build or install folder) and setup/*.pak with the core SQL and default configs.
#   cmake --build build --config RelWithDebInfo --target lonelyice_release
# On Linux and macOS OpenSSL comes from the system and is not copied.
set(LONELYICE_RELEASE_DIR "${CMAKE_BINARY_DIR}/lonelyice-release" CACHE PATH "Where lonelyice_release puts the release")
set(LONELYICE_RUNTIME_DIRS "" CACHE STRING "Additional Windows DLL search directories for release packaging")
if (WIN32)
  # Ship the redistributable MSVC runtime beside the executable, before resolving
  # imports, so an installed system runtime cannot hide a missing release file.
  set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
  include(InstallRequiredSystemLibraries)
  set(RELEASE_RUNTIME_SEARCH "${OPENSSL_RUNTIME_DIR};${LONELYICE_RUNTIME_DIRS}")
  configure_file(cmake/CopyWindowsRuntime.cmake.in "${GEN_DIR}/copy-windows-runtime.cmake" @ONLY)
  set(RELEASE_RUNTIME_COMMAND COMMAND ${CMAKE_COMMAND}
    -DBIN_DIR=$<TARGET_FILE_DIR:lonelyice> -DRELEASE=${LONELYICE_RELEASE_DIR}
    -P "${GEN_DIR}/copy-windows-runtime.cmake")
endif()
if (WIN32)
  set(RELEASE_PLATFORM "windows-x64")
elseif (APPLE)
  if (CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
    set(RELEASE_PLATFORM "macos-arm64")
  else()
    set(RELEASE_PLATFORM "macos-x64")
  endif()
elseif (CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
  set(RELEASE_PLATFORM "linux-arm64")
else()
  set(RELEASE_PLATFORM "linux-x64")
endif()
if (WIN32)
  get_filename_component(OPENSSL_BIN_DIR "${OPENSSL_INCLUDE_DIR}/../bin" ABSOLUTE)
  set(RELEASE_LIBS "common.dll shared.dll database.dll game.dll libcrypto-3-x64.dll libssl-3-x64.dll legacy.dll")
  set(RELEASE_OPTIONAL "legacy.dll")
  set(RELEASE_EXES "worldserver.exe authserver.exe")
  set(RELEASE_SKIP "PATTERN \"*.pdb\" EXCLUDE PATTERN \"*.lib\" EXCLUDE PATTERN \"*.exp\" EXCLUDE PATTERN \"*.ilk\" EXCLUDE")
else()
  set(OPENSSL_BIN_DIR "")
  set(SUFFIX "${CMAKE_SHARED_LIBRARY_SUFFIX}")
  set(RELEASE_LIBS "libcommon${SUFFIX} libshared${SUFFIX} libdatabase${SUFFIX} libgame${SUFFIX}")
  set(RELEASE_OPTIONAL "")
  set(RELEASE_EXES "worldserver authserver")
  set(RELEASE_SKIP "PATTERN \"*.a\" EXCLUDE PATTERN \"*.dSYM\" EXCLUDE")
endif()
string(APPEND RELEASE_SKIP " PATTERN \".cache\" EXCLUDE PATTERN \".disabled\" EXCLUDE")
file(CONFIGURE OUTPUT "${GEN_DIR}/copy-libs.cmake" @ONLY CONTENT
"cmake_minimum_required(VERSION 3.22)
foreach(lib @RELEASE_LIBS@)
  set(found OFF)
  foreach(dir \"\${BIN_DIR}\" \"@OPENSSL_BIN_DIR@\" \"@CMAKE_INSTALL_PREFIX@\" \"@CMAKE_INSTALL_PREFIX@/lib\")
    if (NOT found AND dir AND EXISTS \"\${dir}/\${lib}\")
      file(COPY \"\${dir}/\${lib}\" DESTINATION \"@LONELYICE_RELEASE_DIR@\" FOLLOW_SYMLINK_CHAIN)
      set(found ON)
    endif()
  endforeach()
  if (NOT found AND NOT \"\${lib}\" IN_LIST optional)
    message(FATAL_ERROR \"\${lib} not found\")
  endif()
endforeach()
")
file(CONFIGURE OUTPUT "${GEN_DIR}/copy-extras.cmake" @ONLY CONTENT
"foreach(exe @RELEASE_EXES@)
  if (EXISTS \"\${SRC}/\${exe}\")
    file(COPY \"\${SRC}/\${exe}\" DESTINATION \"\${DST}\")
  endif()
endforeach()
if (EXISTS \"\${SRC}/plugins\")
  file(COPY \"\${SRC}/plugins\" DESTINATION \"\${DST}\" @RELEASE_SKIP@)
endif()
")
add_custom_target(lonelyice_release
  COMMAND ${CMAKE_COMMAND} -E make_directory "${LONELYICE_RELEASE_DIR}/setup"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:lonelyice>" "${LONELYICE_RELEASE_DIR}"
  COMMAND ${CMAKE_COMMAND} -DBIN_DIR=$<TARGET_FILE_DIR:lonelyice> "-Doptional=${RELEASE_OPTIONAL}" -P "${GEN_DIR}/copy-libs.cmake"
  COMMAND ${CMAKE_COMMAND} -DSRC=$<TARGET_FILE_DIR:lonelyice> -DDST=${LONELYICE_RELEASE_DIR} -P "${GEN_DIR}/copy-extras.cmake"
  ${RELEASE_RUNTIME_COMMAND}
  COMMAND ${CMAKE_COMMAND} -DRELEASE=${LONELYICE_RELEASE_DIR} -DPLATFORM=${RELEASE_PLATFORM}
    -DPREFIX=${CMAKE_SHARED_LIBRARY_PREFIX} -DSUFFIX=${CMAKE_SHARED_LIBRARY_SUFFIX}
    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/StampPluginPlatforms.cmake"
  COMMAND "${LONELYICE_RELEASE_DIR}/$<TARGET_FILE_NAME:lonelyice>" --pack "${LONELYICE_CORE_DIR}" "${LONELYICE_RELEASE_DIR}/setup"
  COMMAND "${LONELYICE_RELEASE_DIR}/$<TARGET_FILE_NAME:lonelyice>" --pkg snapshot --settings "${LONELYICE_RELEASE_DIR}/server.yaml"
  DEPENDS lonelyice
  VERBATIM)
set_target_properties(lonelyice_release PROPERTIES FOLDER "server")
get_property(package_targets GLOBAL PROPERTY LONELYICE_PACKAGE_TARGETS)
if (package_targets)
  add_dependencies(lonelyice_release ${package_targets})
endif()
foreach(plugin_source ${LONELYICE_PLUGIN_DIRS})
  if (EXISTS "${plugin_source}/CMakeLists.txt")
    file(READ "${plugin_source}/CMakeLists.txt" plugin_build)
    string(REGEX MATCH "AddPlugin\\(([A-Za-z0-9_-]+)" plugin_match "${plugin_build}")
    if (plugin_match)
      set(plugin_target "${CMAKE_MATCH_1}")
      if (TARGET ${plugin_target})
        add_dependencies(lonelyice_release ${plugin_target})
        if (TARGET ${plugin_target}-files)
          add_dependencies(lonelyice_release ${plugin_target}-files)
        endif()
      endif()
    endif()
  endif()
endforeach()
foreach(core_server worldserver authserver)
  if (TARGET ${core_server})
    add_dependencies(lonelyice_release ${core_server})
  endif()
endforeach()
