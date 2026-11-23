cmake_minimum_required(VERSION 3.16)

project(guitar2midi_player VERSION 0.3.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(MSVC)
  # the real __cplusplus (LVGL.hpp checks it), and UTF-8 sources and strings (the UI's labels)
  add_compile_options(/Zc:__cplusplus /utf-8)
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

enable_testing()

add_subdirectory(core)

find_package(PkgConfig)

# The plugin (LV2, VST3, CLAP, standalone) with DPF. The Windows SDK's OpenGL headers stop at
# version 1.1, and DPF's and the UI's OpenGL code needs Khronos's glext.h: first on every path.
if(WIN32)
  include_directories(BEFORE ${PROJECT_SOURCE_DIR}/external/khronos)
endif()
add_subdirectory(external/dpf)

# LVGL for the plugin's UI, configured by plugin/ui/lv_conf.h
set(LV_BUILD_CONF_PATH "${PROJECT_SOURCE_DIR}/plugin/ui/lv_conf.h" CACHE PATH "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)
add_subdirectory(external/lvgl EXCLUDE_FROM_ALL)
set_target_properties(lvgl PROPERTIES POSITION_INDEPENDENT_CODE ON)  # linked into the plugins' shared objects

add_subdirectory(plugin)
