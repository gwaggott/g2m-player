# The player plugin, built with DPF (external/dpf) into ${CMAKE_BINARY_DIR}/bin:
# g2m.lv2, g2m.vst3, g2m.clap and the standalone g2m (JACK, or the native audio fallback)
set(G2M_PLUGIN_FORMATS lv2 vst3 clap jack CACHE STRING "DPF targets to build")

dpf_add_plugin(g2m
  TARGETS ${G2M_PLUGIN_FORMATS}
  FILES_DSP g2m_plugin.cpp
  FILES_UI g2m_ui.cpp ui/LVGL.cpp
  USE_FILE_BROWSER)

target_include_directories(g2m PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}
                                      ${CMAKE_CURRENT_SOURCE_DIR}/ui)  # LVGL.hpp, DISTRHO_UI_CUSTOM_INCLUDE_PATH
target_link_libraries(g2m PUBLIC lvgl::lvgl)
set_target_properties(dgl-opengl PROPERTIES CXX_EXTENSIONS ON)  # DPF's WebViewImpl.cpp uses typeof
if("lv2" IN_LIST G2M_PLUGIN_FORMATS)
  add_custom_command(TARGET g2m-lv2 POST_BUILD  # after DPF's ttl generator
    COMMAND ${CMAKE_COMMAND} -DFILE=${CMAKE_BINARY_DIR}/bin/g2m.lv2/presets.ttl -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/fix_lv2_presets.cmake
    VERBATIM)
endif()

# Preset files (presets.h), for the plugin and its tests
add_library(g2m_presets STATIC presets.cpp)
set_target_properties(g2m_presets PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(g2m_presets PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} PRIVATE ${PROJECT_SOURCE_DIR}/core/src)  # json.h
target_link_libraries(g2m_presets PUBLIC g2m_core)

# Models bundled with the plugin: their names (file stems) compiled in, the
# files copied into each format's resources folder. Local builds may bundle
# private models; a package must bundle only shareable ones (docs/release-plan.md).
set(G2M_BUNDLED_MODELS "" CACHE STRING "The .g2m files to bundle, the default first (paths relative to the repository root or absolute)")
set(bundled_names "")
set(bundled_files "")
foreach(model ${G2M_BUNDLED_MODELS})
  get_filename_component(file "${model}" ABSOLUTE BASE_DIR "${PROJECT_SOURCE_DIR}")
  if(NOT EXISTS "${file}")
    message(FATAL_ERROR "G2M_BUNDLED_MODELS: ${file} not found")
  endif()
  get_filename_component(name "${file}" NAME_WE)
  string(APPEND bundled_names "    \"${name}\",\n")
  list(APPEND bundled_files "${file}")
endforeach()
list(LENGTH bundled_files bundled_count)
file(CONFIGURE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/g2m_bundled_models.h CONTENT
"// Generated from G2M_BUNDLED_MODELS by plugin/CMakeLists.txt
#pragma once
constexpr int kBundledModelCount = ${bundled_count};
constexpr const char* kBundledModels[] = {
${bundled_names}    nullptr,
};
")
set(bundled_dirs
  ${CMAKE_BINARY_DIR}/bin/g2m.lv2/resources/models
  ${CMAKE_BINARY_DIR}/bin/g2m.vst3/Contents/Resources/models
  ${CMAKE_BINARY_DIR}/bin/g2m-models
  ${CMAKE_BINARY_DIR}/bin/resources/models)
file(REMOVE_RECURSE ${bundled_dirs})
if(bundled_files)
  foreach(dir ${bundled_dirs})
    file(COPY ${bundled_files} DESTINATION ${dir})
  endforeach()
endif()
target_link_libraries(g2m PUBLIC g2m_core g2m_presets)
target_compile_definitions(g2m PUBLIC
  G2M_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
  G2M_VERSION_MINOR=${PROJECT_VERSION_MINOR}
  G2M_VERSION_PATCH=${PROJECT_VERSION_PATCH})

set(G2M_PLUGIN_BIN ${CMAKE_BINARY_DIR}/bin)
set(G2M_LV2_BUNDLE ${G2M_PLUGIN_BIN}/g2m.lv2)
set(G2M_LV2_INSTALL_DIR "$ENV{HOME}/.lv2" CACHE PATH "Where `cmake --install` puts g2m.lv2")
set(G2M_VST3_INSTALL_DIR "$ENV{HOME}/.vst3" CACHE PATH "Where `cmake --install` puts g2m.vst3")
set(G2M_CLAP_INSTALL_DIR "$ENV{HOME}/.clap" CACHE PATH "Where `cmake --install` puts g2m.clap")
install(DIRECTORY ${G2M_LV2_BUNDLE} DESTINATION ${G2M_LV2_INSTALL_DIR} OPTIONAL)
install(DIRECTORY ${G2M_PLUGIN_BIN}/g2m.vst3 DESTINATION ${G2M_VST3_INSTALL_DIR} OPTIONAL)
if(APPLE)  # a bundle (folder) on macOS, a file elsewhere
  install(DIRECTORY ${G2M_PLUGIN_BIN}/g2m.clap DESTINATION ${G2M_CLAP_INSTALL_DIR} OPTIONAL)
else()
  install(FILES ${G2M_PLUGIN_BIN}/g2m.clap DESTINATION ${G2M_CLAP_INSTALL_DIR} OPTIONAL)
endif()
install(DIRECTORY ${G2M_PLUGIN_BIN}/g2m-models DESTINATION ${G2M_CLAP_INSTALL_DIR} OPTIONAL)

add_executable(test_presets test/test_presets.cpp)
target_link_libraries(test_presets PRIVATE g2m_presets g2m_check)
add_test(NAME presets COMMAND test_presets)

# An offline host (lilv) that runs the LV2 bundle and checks its MIDI against the core library
if(PkgConfig_FOUND)
  pkg_check_modules(LILV IMPORTED_TARGET lilv-0)
  pkg_check_modules(LV2 IMPORTED_TARGET lv2)
endif()
if(LILV_FOUND AND LV2_FOUND AND "lv2" IN_LIST G2M_PLUGIN_FORMATS)
  add_executable(test_lv2_plugin test/test_lv2_plugin.cpp)
  target_include_directories(test_lv2_plugin PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})  # the URI, the bundled models
  target_link_libraries(test_lv2_plugin PRIVATE g2m_core g2m_presets g2m_tools_io g2m_check PkgConfig::LILV PkgConfig::LV2)
  target_compile_definitions(test_lv2_plugin PRIVATE
    G2M_LV2_BUNDLE="${G2M_LV2_BUNDLE}/"
    G2M_TEST_DATA="${CMAKE_SOURCE_DIR}/core/test/data")
  # -Wno-mismatched-new-delete: GCC flags allocations.h's replacement operator delete (malloc / free) once inlined here;
  # -Wno-dangling-pointer: lv2_atom_forge keeps a pointer to the caller's frame until it is popped
  target_compile_options(test_lv2_plugin PRIVATE ${G2M_WARNINGS} -Wno-mismatched-new-delete -Wno-dangling-pointer)
  add_dependencies(test_lv2_plugin g2m-lv2)
  add_test(NAME lv2_plugin COMMAND test_lv2_plugin)
else()
  message(STATUS "lilv or the LV2 headers not found: no LV2 plugin test")
endif()

# A zip of the built formats for others to install (packaging/package.cmake):
# build/package/g2m-player-<version>-<os>-<arch>.zip. A build that
# bundles models packages only with G2M_PACKAGE_SHAREABLE on.
option(G2M_PACKAGE_SHAREABLE "Every bundled model may be shared: allow packaging this build" OFF)
if(APPLE AND CMAKE_OSX_ARCHITECTURES MATCHES ";")
  set(package_arch universal)
elseif(CMAKE_OSX_ARCHITECTURES)
  set(package_arch ${CMAKE_OSX_ARCHITECTURES})
else()
  string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" package_arch)
  if(package_arch STREQUAL "amd64")
    set(package_arch x86_64)
  endif()
endif()
string(TOLOWER "${CMAKE_SYSTEM_NAME}" package_os)
if(package_os STREQUAL "darwin")
  set(package_os macos)
endif()
add_custom_target(g2m-package
  COMMAND ${CMAKE_COMMAND} -DBIN=${G2M_PLUGIN_BIN} -DOUT_DIR=${CMAKE_BINARY_DIR}/package
    -DNAME=g2m-player-${PROJECT_VERSION}-${package_os}-${package_arch}
    -DSOURCE=${PROJECT_SOURCE_DIR} "-DMODELS=${G2M_BUNDLED_MODELS}" -DSHAREABLE=${G2M_PACKAGE_SHAREABLE}
    -P ${PROJECT_SOURCE_DIR}/packaging/package.cmake
  VERBATIM)
foreach(format ${G2M_PLUGIN_FORMATS})
  add_dependencies(g2m-package g2m-${format})
endforeach()
