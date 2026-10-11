cmake_minimum_required(VERSION 3.21)

project(tm_windowed_fullscreen VERSION 1.0.0 LANGUAGES CXX)

# TrackMania Forever is 32-bit (Direct3D 9). Always configure with -A Win32.
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "TrackMania Forever is 32-bit: please configure with -A Win32.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
cmake_policy(SET CMP0091 NEW)

# Statically linked runtime (/MT) so no extra MSVC redistributable is required.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_library(tm_windowed_fullscreen SHARED
  src/dllmain.cpp
  src/proxy.cpp
  src/log.cpp
  src/exports.def
)

target_include_directories(tm_windowed_fullscreen PRIVATE src)
target_compile_definitions(tm_windowed_fullscreen PRIVATE
  WIN32_LEAN_AND_MEAN
  NOMINMAX
  _CRT_SECURE_NO_WARNINGS
)
target_compile_options(tm_windowed_fullscreen PRIVATE /W4 /EHsc /permissive- /utf-8)

set_target_properties(tm_windowed_fullscreen PROPERTIES
  OUTPUT_NAME "TMWindowedFullscreen"
  PREFIX ""
)

# --- Release packaging (generated) -----------------------------------------
# The descriptor source of truth lives in the project root:
#   description.yaml   mod metadata (name/author/type/homepage/description)
#   version.yaml       version metadata (executable + changelog)
# After building, generate the release folder consumed by
# packaging/TM Windowed Fullscreen/install-modloader.ps1. The version-level
# descriptor is written as "description.yaml" (the name the ModLoader expects).
# README.md is copied into the release folder as well.
# The whole packaging/ tree is generated - do not edit it by hand.
set(TMWFS_PACKAGE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/packaging/TM Windowed Fullscreen")
add_custom_command(TARGET tm_windowed_fullscreen POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "${TMWFS_PACKAGE_DIR}/${PROJECT_VERSION}"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_CURRENT_SOURCE_DIR}/description.yaml"
          "${TMWFS_PACKAGE_DIR}/description.yaml"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_CURRENT_SOURCE_DIR}/version.yaml"
          "${TMWFS_PACKAGE_DIR}/${PROJECT_VERSION}/description.yaml"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_CURRENT_SOURCE_DIR}/install-modloader.ps1"
          "${TMWFS_PACKAGE_DIR}/install-modloader.ps1"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_CURRENT_SOURCE_DIR}/README.md"
          "${TMWFS_PACKAGE_DIR}/README.md"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "$<TARGET_FILE:tm_windowed_fullscreen>"
          "${TMWFS_PACKAGE_DIR}/${PROJECT_VERSION}/TMWindowedFullscreen.dll"
  COMMENT "Generating release packaging in packaging/TM Windowed Fullscreen"
)