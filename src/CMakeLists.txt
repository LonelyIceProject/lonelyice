# LonelyIce: launcher UI + auth + world in one executable (Windows, Linux, macOS).
# Deps (static SDL3, FreeType, RmlUi, StormLib, miniz, libcurl) are built by tools/build-ui-deps.ps1 (Windows) or
# tools/build-ui-deps.sh (Linux, macOS) into LONELYICE_DEPS.
# The core (LONELYICE_CORE_DIR) is added by the top-level CMakeLists.txt; its targets are linked here.

set(LONELYICE_DEPS "${CMAKE_SOURCE_DIR}/deps" CACHE PATH "Folder with sdl3/, freetype/, rmlui/ and src/rmlui (backends)")
set(LONELYICE_RMLUI_BACKENDS "${LONELYICE_DEPS}/src/rmlui/Backends")

list(APPEND CMAKE_PREFIX_PATH "${LONELYICE_DEPS}/sdl3" "${LONELYICE_DEPS}/freetype" "${LONELYICE_DEPS}/rmlui" "${LONELYICE_DEPS}/stormlib" "${LONELYICE_DEPS}/miniz" "${LONELYICE_DEPS}/curl")
find_package(SDL3 CONFIG QUIET COMPONENTS SDL3-static)
find_package(RmlUi CONFIG QUIET)
find_package(StormLib CONFIG QUIET)
find_package(miniz CONFIG QUIET)
# Static libcurl: its config package adds CURL_STATICLIB and the system libraries it needs.
find_package(CURL CONFIG QUIET)

if (NOT SDL3_FOUND OR NOT RmlUi_FOUND OR NOT StormLib_FOUND OR NOT miniz_FOUND OR NOT CURL_FOUND OR NOT EXISTS "${LONELYICE_RMLUI_BACKENDS}/RmlUi_Renderer_GL3.cpp")
  message(STATUS "LonelyIce: SDL3/RmlUi/StormLib/miniz/libcurl not found in ${LONELYICE_DEPS}, skipping (run tools/build-ui-deps.ps1 or .sh)")
  return()
endif()

set(AUTH_APP_DIR "${LONELYICE_CORE_DIR}/src/server/apps/authserver")
set(WORLD_APP_DIR "${LONELYICE_CORE_DIR}/src/server/apps/worldserver")

file(GLOB_RECURSE LONELYICE_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/*.h")

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
  "${LONELYICE_RMLUI_BACKENDS}/RmlUi_Platform_SDL.cpp"
  "${LONELYICE_RMLUI_BACKENDS}/RmlUi_Renderer_GL3.cpp"
  ${MMAPS_SOURCES})

# The extractor units include the tools' .cpp files; their own headers resolve relative to those files,
# map_extractor additionally includes "loadlib.h" from its loadlib/ folder.
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/tools/MapExtractorUnit.cpp" PROPERTIES
  INCLUDE_DIRECTORIES "${TOOLS_DIR};${TOOLS_DIR}/map_extractor/loadlib")
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/tools/VmapExtractorUnit.cpp" PROPERTIES
  INCLUDE_DIRECTORIES "${TOOLS_DIR}")

# Embedded assets: every file under assets/ becomes a byte array (EmbedAsset.cmake), looked up by relative path.
set(ASSET_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/assets")
file(GLOB_RECURSE ASSET_FILES CONFIGURE_DEPENDS "${ASSET_ROOT}/ui/*" "${ASSET_ROOT}/fonts/*.ttf" "${ASSET_ROOT}/icons/*.rgba" "${ASSET_ROOT}/icons/*.tga" "${ASSET_ROOT}/lang/*.lang")
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

add_executable(lonelyice WIN32 ${LONELYICE_SOURCES})
set_target_properties(lonelyice PROPERTIES OUTPUT_NAME "LonelyIce" FOLDER "server")
add_dependencies(lonelyice revision.h)

target_include_directories(lonelyice PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/common"
  "${CMAKE_CURRENT_SOURCE_DIR}/server"
  "${CMAKE_CURRENT_SOURCE_DIR}/launcher"
  "${CMAKE_CURRENT_SOURCE_DIR}/packages"
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

if (MSVC)
  # WIN32 subsystem but keep the regular main(argc, argv) entry.
  set_property(TARGET lonelyice APPEND_STRING PROPERTY LINK_FLAGS " /ENTRY:mainCRTStartup")
else()
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

install(TARGETS lonelyice DESTINATION "${CMAKE_INSTALL_PREFIX}")

# Release folder: LonelyIce, the core libraries and plain worldserver/authserver, the plugins, the libraries they need
# (taken from the build or install folder) and setup/*.pak with the core SQL and default configs.
#   cmake --build build --config RelWithDebInfo --target lonelyice_release
# On Linux and macOS OpenSSL comes from the system and is not copied.
set(LONELYICE_RELEASE_DIR "${CMAKE_BINARY_DIR}/lonelyice-release" CACHE PATH "Where lonelyice_release puts the release")
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
file(CONFIGURE OUTPUT "${GEN_DIR}/copy-libs.cmake" @ONLY CONTENT
"foreach(lib @RELEASE_LIBS@)
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
  COMMAND "${LONELYICE_RELEASE_DIR}/$<TARGET_FILE_NAME:lonelyice>" --pack "${LONELYICE_CORE_DIR}" "${LONELYICE_RELEASE_DIR}/setup"
  DEPENDS lonelyice
  VERBATIM)
set_target_properties(lonelyice_release PROPERTIES FOLDER "server")
