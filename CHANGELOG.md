# Changelog

All notable changes to Ara240 Vision Examples will be documented in this file.

## [1.3.0] - 2026-11-20 (Q4 2026)

**BSP Version Compatibility:** LF6.18.54_2.2.0

### Added
- **YOLO26 model support** - Integrated YOLOv26 detection model
- **Object tracking** - ByteTrack multi-object tracking with track ID persistence
- **Re-identification (ReID)** - OSNet ReID pipeline with direct-to-gallery EMA and per-camera statistics
- **WebSocket server** - Real-time detection streaming with RTP-timestamp synchronization
- **RTSP output** - Stream processed video with detections via RTSP protocol
- **RTSP clip recording** - Record RTSP stream segments on demand
- **Headless mode** - Run without display for server/embedded deployments (`--headless`)
- **libcamera support** - Native libcamera source for modern camera stack
- **Camera selector** - GUI camera selection and configuration
- **Auto-model selection** - Automatically select model when `-m` is not specified
- **Signal handling** - Graceful shutdown on SIGINT/SIGTERM
- **pkg-config integration** - Use `ara-runtime.pc` for ARA SDK integration

### Changed
- **Tracking improvements** - Enhanced ByteTrack stability and ID retention
- **Camera detection** - Use `device_caps` to exclude V4L2 metadata nodes
- **Window configuration** - Simplified launcher window config and fixed stream crashes
- **Weston auto-detection** - Automatically fall back to headless mode when Weston is unavailable
- **Copyright headers** - Updated NXP copyright to include 2026
- **Documentation** - Re-Identification feature removed from README
- **Test pattern removed** - Removed `--test-pattern` parameter
- **RTSP resolution handling** - Fixed initial resolution and model-fit final resolution

### Fixed
- **Pipeline stability** - Added leaky queue before waylandsink to prevent stalls
- **Headless black-fallback** - Skip black-fallback demotion in headless mode
- **ReID storage issues** - Prevent ReID storage problems during tracking
- **Camera listing** - Properly filter V4L2 metadata nodes
- **Stream crashes** - Fixed crashes in launcher with multiple streams

### Performance
- **WebSocket synchronization** - RTP-timestamp-based detection sync for real-time streaming
- **Track retention** - Improved object tracking stability and ID persistence
- **Pipeline optimization** - Reduced stalls with leaky queue architecture

### Known Issues
- Ara240 with FRDM i.MX 8M Plus boot failures - FRDM i.MX 8M Plus Ara240 initialization may fail or freeze the boot-up process if PCIe ASPM is not disabled. *Workaround:* Install Q4 BSP 6.18.54_2.2.0, stop at U-Boot, append `pcie_aspm=off` to `mmcargs`, save with `saveenv`, and continue boot.

## [1.2.0] - 2026-08-27 (Q3 2026)

**BSP Version Compatibility:** LF6.18.37_2.1.0

### Added
- **YOLO11 model support** - Nano, small, medium, large, extra large variants
- **YOLOxt model support** - Tiny variant
- **Arbitrary input resolutions** - Support for any resolution in multistream pipeline
- **H.265 RTSP stream support** - RTSP streaming with H.265 codec
- **Configurable display window size** - Adjustable window dimensions
- **Resolution controls** - GUI controls for resolution management
- **Threaded video downloads** - Parallel download capability
- **Hardware API** - Dynamically query endpoint count
- **Extended test video set** - Additional sample videos
- **Multi-stream YOLO** - Up to 16 concurrent video streams
- **YOLOv8 model family** - n, s, m, l, x variants
- **YOLOx model family** - t, s, m, l, x variants
- **GStreamer pipeline** - Video pipeline architecture
- **Real-time visualization** - Bounding boxes with classification labels and confidence scores
- **GoPoint integration** - Launcher integration for easy demo access
- **Sample downloads** - Video and model management interface
- **Custom video input** - H.264/MP4 format support

### Changed
- **Video container format** - Switched default from MP4 to TS
- **Stream scaling** - Up to 16 concurrent streams with per-stream resolution tracking
- **Debian packaging** - Updated scripts and streams.json configuration
- **GUI refresh** - Updated controls and visual previews
- **Software architecture** - Improved block diagram documentation
- **README enhancement** - Comprehensive setup and troubleshooting sections
- **Build system** - pkg-config integration for ARA Runtime SDK

### Fixed
- **YOLOX models** - Now available in the GTK GUI
- **RTSP functionality** - Now works and displays bounding boxes correctly
- **GoPoint GUI** - Can now properly close ara2-vision-examples
- **Network connectivity** - DNS configuration guidance
- **System date** - Synchronization for proper downloads

### Performance
- **FPS scaling** - Better performance across workloads
- **Synchronization stability** - Improved on supported hardware

### Known Issues
- Ara240 with FRDM i.MX 8M Plus boot failures - FRDM i.MX 8M Plus Ara240 initialization may fail or freeze the boot-up process if PCIe ASPM is not disabled. *Workaround:* Install Q3 BSP 6.18.37_2.1.0, stop at U-Boot, append `pcie_aspm=off` to `mmcargs`, save with `saveenv`, and continue boot.
- FRDM i.MX 8MP, FRDM i.MX 95, and FRDM i.MX 95 Pro freeze after firmware upgrade - After updating ARA240 firmware and rebooting, the board freezes during boot. *Workaround:* Power-cycle the board after firmware upgrade.
- Application requires models and sample videos to be downloaded before first run - Internet connection required for initial model and video downloads.

## [1.1.0] - 2026-06-08 (Q2 2026)

**BSP Version Compatibility:** LF6.18.20_2.0.0

### Added
- **Multi-stream YOLO** - Initial implementation
- **FRDM i.MX 8M Plus** - Platform support
- **FRDM i.MX 95** - Platform support
- **FRDM i.MX 95 PRO** - Platform support
- **Ara240 16GB M.2 Module** - Hardware integration
- **USB camera input** - Camera support
- **HDMI display output** - 1920x1080 resolution
- **Object detection** - Basic detection with bounding boxes
- **Multistream processing** - 1-8 concurrent video streams
- **Hardware acceleration** - Ara240 DNPU support
- **GStreamer integration** - H.265 support
- **GTK3 GUI launcher** - Graphical interface
- **YOLOv8 and YOLOX models** - Model support
- **Multiple input sources** - V4L2 cameras, RTSP, test patterns
- **Runtime model selection** - Dynamic model management

### Changed
- **BSP version** - Updated to LF6.18.20_2.0.0
- **Documentation** - Improved structure

### Fixed
- **Bug fixes** - Initial stability improvements

## [1.0.0] - 2026-03-27 (Q1 2026)

**BSP Version Compatibility:** LF6.18.2_1.0.0

### Added
- **Initial release** - Ara240 Vision Examples
- **YOLO object detection** - Basic example
- **FRDM i.MX platforms** - Platform support
- **i.MX Linux BSP** - Integration
- **Universal Update Utility (UUU)** - Flashing support
- **Documentation** - Basic setup guide
- **License and SBOM** - Legal files
- **Application Code Hub** - Initial release

### Known Issues
- Limited model support - Initial release
- Single stream operation - Only one stream supported

---

## Version History Summary

| Version | Release Date | Quarter  | BSP Version      |
|---------|--------------|----------|------------------|
| 1.0.0   | 2026-03-27   | Q1 2026  | LF6.18.2_1.0.0   |
| 1.1.0   | 2026-06-08   | Q2 2026  | LF6.18.20_2.0.0  |
| 1.2.0   | 2026-08-27   | Q3 2026  | LF6.18.37_2.1.0  |
| 1.3.0   | 2026-11-20   | Q4 2026  | LF6.18.54_2.2.0  |

## Links

- [NXP i.MX Software](https://www.nxp.com/design/design-center/software/embedded-software/i-mx-software/embedded-linux-for-i-mx-applications-processors:IMXLINUX)
- [GoPoint for i.MX](https://www.nxp.com/design/design-center/software/i-mx-developer-resources/gopoint-for-i-mx-applications-processors:GOPOINT)
- [Ara240 User Guide](https://www.nxp.com/docs/en/user-guide/UG10166.pdf)
