cmake_minimum_required(VERSION 3.16)
project(LonelyIceYamlSettingsTests LANGUAGES CXX)

# This isolated target uses production YAML/model code without building the game server or graphical UI.
get_filename_component(LONELYICE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(LONELYICE_CORE_DIR "${LONELYICE_ROOT}/../../azerothcore" CACHE PATH "Core tree providing fkYAML and fmt headers")
if(NOT EXISTS "${LONELYICE_CORE_DIR}/deps/fkYAML/fkYAML/node.hpp" OR
   NOT EXISTS "${LONELYICE_CORE_DIR}/deps/fmt/include/fmt/format.h")
  message(FATAL_ERROR "Set LONELYICE_CORE_DIR to an AzerothCore source tree containing fkYAML and fmt")
endif()

add_executable(lonelyice_yaml_settings_test
  settings-model-yaml.cpp
  "${LONELYICE_ROOT}/src/launcher/SettingsModel.cpp"
  "${LONELYICE_ROOT}/src/launcher/LauncherSettings.cpp"
  "${LONELYICE_ROOT}/src/launcher/ProfileConfig.cpp"
  "${LONELYICE_ROOT}/src/launcher/ConfFile.cpp"
  "${LONELYICE_ROOT}/src/launcher/Plugins.cpp")
target_compile_features(lonelyice_yaml_settings_test PRIVATE cxx_std_20)
target_compile_definitions(lonelyice_yaml_settings_test PRIVATE FMT_HEADER_ONLY)
target_include_directories(lonelyice_yaml_settings_test PRIVATE
  "${LONELYICE_ROOT}/src/launcher"
  "${LONELYICE_ROOT}/src/common"
  "${LONELYICE_CORE_DIR}/deps/fkYAML"
  "${LONELYICE_CORE_DIR}/deps/fmt/include")
enable_testing()
add_test(NAME yaml_settings_roundtrip COMMAND lonelyice_yaml_settings_test)
