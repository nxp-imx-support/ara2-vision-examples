#
# Copyright 2025-2026 NXP
#
# SPDX-License-Identifier: BSD-3-Clause
#
# File: CMakeLists.txt
#

cmake_minimum_required(VERSION 3.10)

project(multistream_yolo)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find required packages
find_package(PkgConfig REQUIRED)

pkg_check_modules(GSTREAMER REQUIRED gstreamer-1.0)
pkg_check_modules(GSTREAMER_VIDEO REQUIRED gstreamer-video-1.0)
pkg_check_modules(CAIRO REQUIRED cairo)
pkg_check_modules(GTK3 REQUIRED gtk+-3.0)

# Check for RTSDK_PATH environment variable (REQUIRED)
if(NOT DEFINED ENV{RTSDK_PATH})
  message(FATAL_ERROR "RTSDK_PATH environment variable must be set")
endif()

set(RTSDK_PATH $ENV{RTSDK_PATH})
message(STATUS "RTSDK_PATH: ${RTSDK_PATH}")

set(RTSDK_INCLUDE_DIR "${RTSDK_PATH}/usr/include/sdk_ara")
set(RTSDK_LIB_DIR "${RTSDK_PATH}/usr/lib")
set(RTSDK_LIB_FILE "${RTSDK_LIB_DIR}/libaraclient_aarch64.so")

# Check if dvapi.h exists (REQUIRED)
if(NOT EXISTS "${RTSDK_INCLUDE_DIR}/dvapi.h")
  message(FATAL_ERROR "dvapi.h not found at: ${RTSDK_INCLUDE_DIR}/dvapi.h")
endif()

message(STATUS "dvapi.h found at: ${RTSDK_INCLUDE_DIR}/dvapi.h")

# Check if libaraclient_aarch64.so exists (REQUIRED)
if(NOT EXISTS "${RTSDK_LIB_FILE}")
  message(FATAL_ERROR "libaraclient_aarch64.so not found at: ${RTSDK_LIB_FILE}")
endif()

message(STATUS "libaraclient_aarch64.so found at: ${RTSDK_LIB_FILE}")

# Source files for main application
set(SOURCES
  src/multistream_yolo.cpp
  src/stream_config.cpp
  src/simple_json_parser.cpp
)

# Create main executable
add_executable(multistream_yolo ${SOURCES})

# Include directories for main application
target_include_directories(multistream_yolo PRIVATE
  ${GSTREAMER_INCLUDE_DIRS}
  ${GSTREAMER_VIDEO_INCLUDE_DIRS}
  ${CAIRO_INCLUDE_DIRS}
  ${RTSDK_INCLUDE_DIR}
)

# Link directories
target_link_directories(multistream_yolo PRIVATE
  ${RTSDK_LIB_DIR}
)

# Link libraries for main application
target_link_libraries(multistream_yolo
  ${GSTREAMER_LIBRARIES}
  ${GSTREAMER_VIDEO_LIBRARIES}
  ${CAIRO_LIBRARIES}
  araclient_aarch64
)

# Create launcher executable
add_executable(launcher_multistream_yolo src/launcher_multistream_yolo.cpp)

# Include directories for launcher
target_include_directories(launcher_multistream_yolo PRIVATE
  ${GTK3_INCLUDE_DIRS}
)

# Link libraries for launcher
target_link_libraries(launcher_multistream_yolo
  ${GTK3_LIBRARIES}
)

# Installation
install(TARGETS multistream_yolo launcher_multistream_yolo
  RUNTIME DESTINATION bin
)
