# LonelyIce: launcher UI + auth + world in one exe (Windows only).
# UI deps (static SDL3, FreeType, RmlUi) are built by tools/build-ui-deps.ps1 into LONELYICE_DEPS.

set(LONELYICE_DEPS "${CMAKE_SOURCE_DIR}/../deps" CACHE PATH "Folder with sdl3/, freetype/, rmlui/ and src/rmlui (backends)")
set(LONELYICE_RMLUI_BACKENDS "${LONELYICE_DEPS}/src/rmlui/Backends")

list(APPEND CMAKE_PREFIX_PATH "${LONELYICE_DEPS}/sdl3" "${LONELYICE_DEPS}/freetype" "${LONELYICE_DEPS}/rmlui")
find_package(SDL3 CONFIG QUIET COMPONENTS SDL3-static)
find_package(RmlUi CONFIG QUIET)

if (NOT SDL3_FOUND OR NOT RmlUi_FOUND OR NOT EXISTS "${LONELYICE_RMLUI_BACKENDS}/RmlUi_Renderer_GL3.cpp")
  message(STATUS "LonelyIce: SDL3/RmlUi not found in ${LONELYICE_DEPS}, skipping (run tools/build-ui-deps.ps1)")
  return()
endif()

set(AUTH_APP_DIR "${CMAKE_SOURCE_DIR}/src/server/apps/authserver")
set(WORLD_APP_DIR "${CMAKE_SOURCE_DIR}/src/server/apps/worldserver")

file(GLOB_RECURSE LONELYICE_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/*.h")

set(TOOLS_DIR "${CMAKE_SOURCE_DIR}/src/tools")
set(MMAPS_SOURCES
  "${TOOLS_DIR}/mmaps_generator/Config.cpp"
  "${TOOLS_DIR}/mmaps_generator/IntermediateValues.cpp"
  "${TOOLS_DIR}/mmaps_generator/MapBuilder.cpp"
  "${TOOLS_DIR}/mmaps_generator/TerrainBuilder.cpp")

list(APPEND LONELYICE_SOURCES
  "${AUTH_APP_DIR}/Server/AuthSession.cpp"
  "${AUTH_APP_DIR}/Authentication/AuthCodes.cpp"
  "${WORLD_APP_DIR}/ACSoap/ACSoap.cpp"
  "${LONELYICE_RMLUI_BACKENDS}/RmlUi_Platform_SDL.cpp"
  "${LONELYICE_RMLUI_BACKENDS}/RmlUi_Renderer_GL3.cpp"
  ${MMAPS_SOURCES})

# The extractor units include the tools' .cpp files; their own headers resolve relative to those files,
# map_extractor additionally includes "loadlib.h" from its loadlib/ folder.
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/tools/MapExtractorUnit.cpp" PROPERTIES
  INCLUDE_DIRECTORIES "${TOOLS_DIR};${TOOLS_DIR}/map_extractor/loadlib")
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/tools/VmapExtractorUnit.cpp" PROPERTIES
  INCLUDE_DIRECTORIES "${TOOLS_DIR}")

# Embedded assets: every file under assets/ becomes an RCDATA resource, looked up by relative path.
set(ASSET_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/assets")
file(GLOB_RECURSE ASSET_FILES CONFIGURE_DEPENDS "${ASSET_ROOT}/ui/*" "${ASSET_ROOT}/fonts/*.ttf" "${ASSET_ROOT}/icons/*.rgba" "${ASSET_ROOT}/icons/*.tga")
# Icon 1 is what Explorer and the taskbar show for the exe.
set(ASSET_RC "1 ICON \"${ASSET_ROOT}/icons/lonelyice-round.ico\"\n")
set(ASSET_INDEX "")
set(ASSET_ID 1000)
foreach(ASSET ${ASSET_FILES})
  file(RELATIVE_PATH REL "${ASSET_ROOT}" "${ASSET}")
  string(APPEND ASSET_RC "${ASSET_ID} RCDATA \"${ASSET}\"\n")
  string(APPEND ASSET_INDEX "    { \"${REL}\", ${ASSET_ID} },\n")
  math(EXPR ASSET_ID "${ASSET_ID} + 1")
endforeach()
string(APPEND ASSET_RC "${ASSET_ID} RCDATA \"${TOOLS_DIR}/mmaps_generator/mmaps-config.yaml\"\n")
string(APPEND ASSET_INDEX "    { \"tools/mmaps-config.yaml\", ${ASSET_ID} },\n")
list(APPEND ASSET_FILES "${TOOLS_DIR}/mmaps_generator/mmaps-config.yaml")
list(LENGTH ASSET_FILES ASSET_COUNT)
set(GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(CONFIGURE OUTPUT "${GEN_DIR}/assets.rc" CONTENT "${ASSET_RC}")
file(CONFIGURE OUTPUT "${GEN_DIR}/AssetIndex.cpp" CONTENT
"#include <cstddef>
namespace LonelyIce
{
    struct AssetEntry { char const* path; int id; };
    extern AssetEntry const g_assetIndex[] = {
${ASSET_INDEX}    { nullptr, 0 }
    };
    extern std::size_t const g_assetCount = ${ASSET_COUNT};
}
")
list(APPEND LONELYICE_SOURCES "${GEN_DIR}/assets.rc" "${GEN_DIR}/AssetIndex.cpp")
set_source_files_properties("${GEN_DIR}/assets.rc" PROPERTIES OBJECT_DEPENDS "${ASSET_FILES};${ASSET_ROOT}/icons/lonelyice-round.ico")

add_executable(lonelyice WIN32 ${LONELYICE_SOURCES})
set_target_properties(lonelyice PROPERTIES OUTPUT_NAME "LonelyIce" FOLDER "server")
add_dependencies(lonelyice revision.h)

target_include_directories(lonelyice PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/common"
  "${CMAKE_CURRENT_SOURCE_DIR}/server"
  "${CMAKE_CURRENT_SOURCE_DIR}/launcher"
  "${AUTH_APP_DIR}/Server"
  "${AUTH_APP_DIR}/Authentication"
  "${WORLD_APP_DIR}/ACSoap"
  "${LONELYICE_RMLUI_BACKENDS}")

target_compile_definitions(lonelyice PRIVATE RMLUI_SDL_VERSION_MAJOR=3)

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
    RmlUi::RmlUi
    SDL3::SDL3-static
    version
    psapi
    winmm
    shell32)

# WIN32 subsystem but keep the regular main(argc, argv) entry.
set_property(TARGET lonelyice APPEND_STRING PROPERTY LINK_FLAGS " /ENTRY:mainCRTStartup")

if (USE_REAL_LIBSIDECAR AND LIBSIDECAR_SHARED_FILE)
  add_custom_command(TARGET lonelyice POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${LIBSIDECAR_SHARED_FILE}" "$<TARGET_FILE_DIR:lonelyice>")
endif()

install(TARGETS lonelyice DESTINATION "${CMAKE_INSTALL_PREFIX}")

# Release folder: the exe, the DLLs it needs (taken from the install folder) and setup/*.pak with SQL and default configs.
#   cmake --build build --config RelWithDebInfo --target lonelyice_release
set(LONELYICE_RELEASE_DIR "${CMAKE_BINARY_DIR}/lonelyice-release" CACHE PATH "Where lonelyice_release puts the release")
file(CONFIGURE OUTPUT "${GEN_DIR}/copy-dlls.cmake" @ONLY CONTENT
"foreach(dll libcrypto-3-x64.dll libssl-3-x64.dll legacy.dll libmysql.dll lua51.dll)
  set(found OFF)
  foreach(dir \"\${BIN_DIR}\" \"@CMAKE_INSTALL_PREFIX@\")
    if (NOT found AND dir AND EXISTS \"\${dir}/\${dll}\")
      file(COPY \"\${dir}/\${dll}\" DESTINATION \"@LONELYICE_RELEASE_DIR@\")
      set(found ON)
    endif()
  endforeach()
  if (NOT found)
    message(FATAL_ERROR \"\${dll} not found\")
  endif()
endforeach()
")
add_custom_target(lonelyice_release
  COMMAND ${CMAKE_COMMAND} -E make_directory "${LONELYICE_RELEASE_DIR}/setup"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:lonelyice>" "${LONELYICE_RELEASE_DIR}"
  COMMAND ${CMAKE_COMMAND} -DBIN_DIR=$<TARGET_FILE_DIR:lonelyice> -P "${GEN_DIR}/copy-dlls.cmake"
  COMMAND "${LONELYICE_RELEASE_DIR}/LonelyIce.exe" --pack "${CMAKE_SOURCE_DIR}" "${LONELYICE_RELEASE_DIR}/setup"
  DEPENDS lonelyice
  VERBATIM)
set_target_properties(lonelyice_release PROPERTIES FOLDER "server")
