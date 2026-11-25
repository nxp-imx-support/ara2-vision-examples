#
# Copyright 2025-2026 NXP
#
# SPDX-License-Identifier: BSD-3-Clause
#
# File: CMakeLists.txt
#

cmake_minimum_required(VERSION 3.10)

# Set policy to suppress FindBoost warning (CMake 3.30+)
if(POLICY CMP0167)
  cmake_policy(SET CMP0167 OLD)
endif()

project(multistream_yolo)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find required packages
find_package(PkgConfig REQUIRED)
find_package(Boost REQUIRED)

pkg_check_modules(GSTREAMER REQUIRED gstreamer-1.0)
pkg_check_modules(GSTREAMER_VIDEO REQUIRED gstreamer-video-1.0)
pkg_check_modules(GSTREAMER_RTP REQUIRED gstreamer-rtp-1.0)
pkg_check_modules(CAIRO REQUIRED cairo)
pkg_check_modules(GTK3 REQUIRED gtk+-3.0)

# OpenCV: used by the ReID crop store (resize, cvtColor, Laplacian, imwrite).
pkg_check_modules(OPENCV REQUIRED opencv4)
set(REQ_OPENCV_LIBS opencv_core opencv_imgproc opencv_imgcodecs)

# Local development support: Allow using locally extracted .deb packages
# Set ARA_RUNTIME_LOCAL_PATH to point to your staging directory
if(DEFINED ENV{ARA_RUNTIME_LOCAL_PATH})
  message(STATUS "Using local ARA Runtime from: $ENV{ARA_RUNTIME_LOCAL_PATH}")
  set(ENV{PKG_CONFIG_SYSROOT_DIR} "$ENV{ARA_RUNTIME_LOCAL_PATH}")
  set(ENV{PKG_CONFIG_PATH} "$ENV{ARA_RUNTIME_LOCAL_PATH}/usr/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}")
  message(STATUS "PKG_CONFIG_SYSROOT_DIR: $ENV{PKG_CONFIG_SYSROOT_DIR}")
  message(STATUS "PKG_CONFIG_PATH: $ENV{PKG_CONFIG_PATH}")
else()
  message(STATUS "Using ARA Runtime from SDK sysroot")
endif()

# ARA SDK via pkg-config
# Cross-compilation: PKG_CONFIG_PATH / PKG_CONFIG_SYSROOT_DIR must point to the
# target sysroot (set automatically by the Yocto toolchain environment script).
pkg_check_modules(ARA_RT REQUIRED ara-runtime)

# Source files for main application
set(SOURCES
  src/multistream_yolo.cpp
  src/detection_ws_server.cpp
  src/stream_config.cpp
  src/simple_json_parser.cpp
  src/reid_crop_store.cpp
  src/ara_runtime.cpp
  src/ara_reid_embedder.cpp
  src/identity_manager.cpp
)

# Create main executable
add_executable(multistream_yolo ${SOURCES})

# Include directories for main application
target_include_directories(multistream_yolo PRIVATE
  ${GSTREAMER_INCLUDE_DIRS}
  ${GSTREAMER_VIDEO_INCLUDE_DIRS}
  ${GSTREAMER_RTP_INCLUDE_DIRS}
  ${CAIRO_INCLUDE_DIRS}
  ${OPENCV_INCLUDE_DIRS}
  ${ARA_RT_INCLUDE_DIRS}
  ${Boost_INCLUDE_DIRS}
)

# Link directories
target_link_directories(multistream_yolo PRIVATE
  ${ARA_RT_LIBRARY_DIRS}
  ${OPENCV_LIBRARY_DIRS}
)

# Link libraries for main application
target_link_libraries(multistream_yolo
  ${GSTREAMER_LIBRARIES}
  ${GSTREAMER_VIDEO_LIBRARIES}
  ${GSTREAMER_RTP_LIBRARIES}
  ${CAIRO_LIBRARIES}
  ${REQ_OPENCV_LIBS}
  ${ARA_RT_LIBRARIES}
  dl
  pthread
)

# Create launcher executable
add_executable(launcher_multistream_yolo src/launcher_multistream_yolo.cpp)

# Include directories for launcher
target_include_directories(launcher_multistream_yolo PRIVATE
  ${GTK3_INCLUDE_DIRS}
  ${ARA_RT_INCLUDE_DIRS}
)

# Link directories for launcher
target_link_directories(launcher_multistream_yolo PRIVATE
  ${ARA_RT_LIBRARY_DIRS}
)

# Link libraries for launcher
target_link_libraries(launcher_multistream_yolo
  ${GTK3_LIBRARIES}
  ${ARA_RT_LIBRARIES}
)

# Installation
install(TARGETS multistream_yolo launcher_multistream_yolo
  RUNTIME DESTINATION bin
)
