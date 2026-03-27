#
# Copyright 2026 NXP
#
# NXP Proprietary. This software is owned or controlled by NXP and may only be
# used strictly in accordance with the applicable license terms. By expressly
# accepting such terms or by downloading, installing, activating and/or
# otherwise using the software, you are agreeing that you have read, and that
# you agree to comply with and are bound by, such license terms. If you do not
# agree to be bound by the applicable license terms, then you may not retain,
# install, activate or otherwise use the software.
#

cmake_minimum_required(VERSION 3.10)

project(multistream_yolov8)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find required packages
find_package(PkgConfig REQUIRED)

pkg_check_modules(GSTREAMER REQUIRED gstreamer-1.0)
pkg_check_modules(GSTREAMER_VIDEO REQUIRED gstreamer-video-1.0)
pkg_check_modules(CAIRO REQUIRED cairo)
pkg_check_modules(GLIB REQUIRED glib-2.0)

# Include directories
include_directories(
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${GSTREAMER_INCLUDE_DIRS}
  ${GSTREAMER_VIDEO_INCLUDE_DIRS}
  ${CAIRO_INCLUDE_DIRS}
  ${GLIB_INCLUDE_DIRS}
)

# Source files
set(SOURCES 
  src/multistream_yolov8.cpp
)

# Executable
add_executable(multistream_yolov8 ${SOURCES})

# Link libraries
target_link_libraries(
  multistream_yolov8 
  ${GSTREAMER_LIBRARIES} 
  ${GSTREAMER_VIDEO_LIBRARIES}
  ${CAIRO_LIBRARIES} 
  ${GLIB_LIBRARIES} 
  pthread
)

# Installation
install(TARGETS multistream_yolov8 DESTINATION bin)
