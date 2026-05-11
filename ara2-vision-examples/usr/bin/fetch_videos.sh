#!/bin/bash

# Copyright 2026 NXP
#
# NXP Proprietary. This software is owned or controlled by NXP and may only be
# used strictly in accordance with the applicable license terms. By expressly
# accepting such terms or by downloading, installing, activating and/or
# otherwise using the software, you are agreeing that you have read, and that
# you agree to comply with and are bound by, such license terms. If you do not
# agree to be bound by the applicable license terms, then you may not retain,
# install, activate or otherwise use the software.

# ------------------------------------------------------------------------------
# PIXABAY CONTENT LICENSE
# ------------------------------------------------------------------------------
# The sample videos downloaded by this script are sourced from Pixabay and are
# subject to the Pixabay Content License:
# https://pixabay.com/service/license-summary/
#
# Key terms:
# - Content is free for commercial and non-commercial use
# - No attribution required, but appreciated
# - Content cannot be sold or distributed as-is (standalone)
# - Content is used here for demonstration purposes in conjunction with
#   NXP's object detection software
# - Pixabay and content owners are not affiliated with or endorsed by NXP
#
# For full license terms, visit: https://pixabay.com/service/license-summary/
# ------------------------------------------------------------------------------

set -e

OUTPUT_DIR="/usr/share/ara2-vision-examples/sample_videos"

# ---------------------------
# Function: Display License
# ---------------------------
show_license() {
  # Check if terminal supports colors
  if command -v tput >/dev/null 2>&1 && tput setaf 1 >/dev/null 2>&1; then
    local GREEN=$(tput setaf 2 setab 0)
    local RED=$(tput setaf 1 setab 0)
    local NC=$(tput sgr0) #Turn off all attributes
  else
    local GREEN=''
    local RED=''
    local NC=''
  fi

  cat << EOF
-------------------------------------------------------------------------------
                    PIXABAY CONTENT LICENSE INFORMATION
-------------------------------------------------------------------------------

SOURCE: Pixabay (https://pixabay.com/)
LICENSE: Pixabay Content License
LICENSE URL: https://pixabay.com/service/license-summary/

VIDEOS DOWNLOADED BY THIS SCRIPT:
----------------------------------
  video_0.mp4 - https://cdn.pixabay.com/video/2023/12/06/192281-892475127_large.mp4
  video_1.mp4 - https://cdn.pixabay.com/video/2015/12/11/1643-148614430_medium.mp4
  video_2.mp4 - https://cdn.pixabay.com/video/2024/02/17/200839-913897710_large.mp4
  video_3.mp4 - https://cdn.pixabay.com/video/2020/06/18/42479-431756043_large.mp4
  video_4.mp4 - https://cdn.pixabay.com/video/2016/05/12/3133-166335900_large.mp4
  video_5.mp4 - https://cdn.pixabay.com/video/2022/10/31/137317-766503093_large.mp4
  video_6.mp4 - https://cdn.pixabay.com/video/2015/10/16/1046-142621379_large.mp4
  video_7.mp4 - https://cdn.pixabay.com/video/2025/04/23/273921_large.mp4

LICENSE SUMMARY:
----------------

  ${GREEN}[+]${NC} Free for commercial and non-commercial use
  ${GREEN}[+]${NC} No attribution required (but appreciated)
  ${GREEN}[+]${NC} Modification and derivative works allowed
  ${GREEN}[+]${NC} Use in software applications permitted
  ${RED}[-]${NC} Cannot be sold or redistributed as standalone content
  ${RED}[-]${NC} Cannot be used to create competing stock media services

USAGE IN THIS PROJECT:
----------------------
These videos are used for demonstration purposes in NXP's ARA2 Vision Examples,
specifically for the YOLO multi-stream object detection demo. The videos are
processed (resized, re-encoded) to optimize performance.

ATTRIBUTION:
------------
While not required, we acknowledge and thank the Pixabay community and content
creators who made these videos available.

DISCLAIMER:
-----------
Pixabay and the content creators are not affiliated with, sponsored by, or
endorsed by NXP. NXP is solely responsible for this software and its use of
Pixabay content.

For complete license terms, visit:
https://pixabay.com/service/license-summary/

------------------------------------------------------------------------------
EOF
}

# ---------------------------
# Function: Display Help
# ---------------------------
show_help() {
    cat << EOF
Usage: fetch_videos [OPTIONS]

Download and process sample videos from Pixabay for ARA2 Vision Examples.

OPTIONS:
  --help, -h        Display this help message and exit

DESCRIPTION:
  This script downloads 8 sample videos from Pixabay and processes them
  for use with the multistream_yolo demo application. Videos are:
  - Resized to 640x360 resolution
  - Re-encoded to H.264 at 30fps
  - Saved to ${OUTPUT_DIR}

  Pixabay license information is displayed after the download completes.

REQUIREMENTS:
  - Must be run as root (for directory permissions)
  - Internet connection required
  - GStreamer tools must be installed

EXAMPLES:
  # Download and process videos
  fetch_videos.sh

  # Show help
  fetch_videos.sh --help

EOF
}

# ---------------------------
# Parse Arguments
# ---------------------------
if [[ "$1" == "--help" ]] || [[ "$1" == "-h" ]]; then
    show_help
    exit 0
fi

echo "--- Validating output directory ---"
if [[ ! -d "${OUTPUT_DIR}" ]]; then
    echo "Directory ${OUTPUT_DIR} does not exist. Creating it..."
    mkdir -p "${OUTPUT_DIR}"
    echo "Directory created successfully."
else
    echo "Directory ${OUTPUT_DIR} already exists."
fi

echo "--- Validating internet connection ---"
if ping -c 4 -q cdn.pixabay.com > /dev/null 2>&1; then
    echo "Ping successful"
else
    echo "Internet connection fail"
    exit 1
fi

# ---------------------------
# Video URLs
# ---------------------------
URLS=(
    "https://cdn.pixabay.com/video/2023/12/06/192281-892475127_large.mp4"
    "https://cdn.pixabay.com/video/2015/12/11/1643-148614430_medium.mp4"
    "https://cdn.pixabay.com/video/2024/02/17/200839-913897710_large.mp4"
    "https://cdn.pixabay.com/video/2020/06/18/42479-431756043_large.mp4"
    "https://cdn.pixabay.com/video/2016/05/12/3133-166335900_large.mp4"
    "https://cdn.pixabay.com/video/2022/10/31/137317-766503093_large.mp4"
    "https://cdn.pixabay.com/video/2015/10/16/1046-142621379_large.mp4"
    "https://cdn.pixabay.com/video/2025/04/23/273921_large.mp4"
)

# ---------------------------
# Counters
# ---------------------------
TOTAL_VIDEOS=${#URLS[@]}
SUCCESS_COUNT=0
FAILED_COUNT=0

# ---------------------------
# Download, rename, process
# ---------------------------
INDEX=0

for URL in "${URLS[@]}"; do
    INPUT="video${INDEX}.mp4"
    OUTPUT="video_${INDEX}.mp4"

    echo "--- Downloading video ${INDEX} ---"
    if wget -q -O "${INPUT}" "${URL}"; then
        echo "--- Processing video ${INDEX} ---"
        if gst-launch-1.0 -q --no-position \
            filesrc location="${INPUT}" typefind=true ! \
            decodebin3 ! \
            imxvideoconvert_g2d ! \
            video/x-raw,width=640,height=360 ! \
            videorate ! \
            video/x-raw,framerate=30/1 ! \
            v4l2h264enc ! \
            h264parse ! \
            filesink location="${OUTPUT_DIR}/${OUTPUT}" > /dev/null 2>&1; then 
            
            SUCCESS_COUNT=$((SUCCESS_COUNT + 1))
            echo "--- Done: ${OUTPUT} ---"
        else
            FAILED_COUNT=$((FAILED_COUNT + 1))
            echo "--- Failed to process: ${OUTPUT} ---"
        fi
        
        rm -f "${INPUT}"
    else
        FAILED_COUNT=$((FAILED_COUNT + 1))
        echo "--- Failed to download video ${INDEX} ---"
    fi
    
    INDEX=$((INDEX + 1))
done

cat << EOF
----------------------------------------
--- Summary ---
Total videos: ${TOTAL_VIDEOS}
Successfully processed: ${SUCCESS_COUNT}
Failed: ${FAILED_COUNT}
----------------------------------------

Videos saved to: ${OUTPUT_DIR}

EOF

show_license

if [[ ${SUCCESS_COUNT} -eq ${TOTAL_VIDEOS} ]]; then
    echo "--- All videos processed successfully ---"
    exit 0
else
    echo "--- Some videos failed to process ---"
    exit 1
fi