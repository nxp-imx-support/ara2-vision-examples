<div align="center">

# NXP Ara240 Vision Examples

[![License](https://img.shields.io/badge/License-Proprietary-red)](./LICENSE.txt)
[![Platforms](https://img.shields.io/badge/Platforms-FRDM_i.MX_8M_Plus_|_FRDM_i.MX_95-blue)](https://www.nxp.com/products/processors-and-microcontrollers/arm-processors/i-mx-applications-processors:IMX_HOME)
[![Language](https://img.shields.io/badge/C++-00599C?logo=cplusplus)](https://isocpp.org/)
[![AI/ML](https://img.shields.io/badge/AI/ML-Vision-orange)](https://www.nxp.com/docs/en/user-guide/UG10166.pdf)
[![BSP](https://img.shields.io/badge/BSP_>=-LF6.18.2--1.0.0-purple.svg?logo=linux&logoColor=white)](https://www.nxp.com/design/design-center/software/embedded-software/i-mx-software/embedded-linux-for-i-mx-applications-processors:IMXLINUX)

---

</div>

## 📖 Project Description

This repository contains comprehensive vision examples for i.MX processors with Ara240 DNPU acceleration. It demonstrates real-time video processing with AI/ML inference capabilities including object detection, classification, pose estimation, and semantic segmentation.

## 💻 Supported Platforms

| Platform                                                                                                    | Supported |
| ----------------------------------------------------------------------------------------------------------- | :-------: |
| [FRDM i.MX 8M Plus](https://www.nxp.com/design/design-center/development-boards-and-designs/FRDM-IMX8MPLUS) |     ✅     |
| [FRDM i.MX 95](https://www.nxp.com/design/design-center/development-boards-and-designs/FRDM-IMX95)          |     ✅     |

## 📋 Requirements

### 💻 Software

- Ara240 Runtime SDK installed on target
- [Embedded Linux for i.MX](https://www.nxp.com/design/design-center/software/embedded-software/i-mx-software/embedded-linux-for-i-mx-applications-processors:IMXLINUX) (>= LF6.18.2_1.0.0)
- [ara2-vision-example.deb](https://www.nxp.com/webapp/sps/download/license.jsp?colCode=ARA2-VISION-EXAMPLES-1.0&appType=file1&DOWNLOAD_ID=null) (optional, build instructions available in this repository)


## 🔨 Build `ara2-vision-examples.deb` package

1.  Clone the repository on your host PC:

    ```bash
    git clone https://github.com/nxp-imx-support/ara2-vision-examples.git
    ```

2. Change directory to the repository and run the following command. Make sure you have the NXP toolchain installed for the FRDM BSP version you need. Steps to build the toolchain are available at [iMX Linux User's Guide](https://www.nxp.com/docs/en/user-guide/UG10163.pdf)

    ```bash
    bash build.sh <path_to_your_toolchain>
    ```

## 🧰 Installation on i.MX

**NOTE:** Make sure the Ara240 Runtime SDK is installed in the FRDM i.MX system before moving forward.

1. Copy the `ara2-vision-examples.deb` to the FRDM i.MX board. You can use `scp` command as below if you know the IP of the target board:

   ```bash
   scp ara2-vision-examples.deb root@<ip_addr>:
   ```

2. Install the package with the following command:

   ```bash
   dpkg -i ara2-vision-examples.deb
   ```
**NOTE:** If you downloaded the pre-built `.deb` package from NXP.COM, the package name will includes the version. Use the actual package name in the command above. `dpkg -i ara2-vision-examples-<version>.deb`

### 🗑️ Uninstalling the Package

To remove the package while keeping configuration files:

```bash
dpkg -r ara2-vision-examples
```

To completely remove the package including all configuration files:

```bash
dpkg -P ara2-vision-examples
```

### ✅ Verifying Installation

Check if the package is installed:

```bash
dpkg -l | grep ara2-vision-examples
```

View package information:

```bash
dpkg -s ara2-vision-examples
```

List installed files:

```bash
dpkg -L ara2-vision-examples
```

## 🎯 Available examples

| Snapshot | Name | Platforms | Implementation | Model |
| :---: | --- | --- | :---: | :---: |
| <a href="./tasks/object-detection/yolov8n/multistream-gstreamer/README.md"><img src="./data/yolov8nExample.webp" width="150" alt="multistream_yolov8n"></a> | [multistream_yolov8](./tasks/object-detection/yolov8n/multistream-gstreamer/README.md) | FRDM i.MX 8M Plus<br>FRDM i.MX 95 | C++ | YOLOv8n (640×640)<br>YOLOv8s (640×640)<br>YOLOv8m (640×640)<br>YOLOv8l (640×640)<br>YOLOv8x (640×640) |

## 🔧 Troubleshooting

### 🚫 Application won't start
- Ensure `rt-sdk-ara2` is installed: `dpkg -l | grep rt-sdk-ara2`
- Check if the model is downloaded: `ls /usr/share/cnn/detection/yolov8n/`
- Verify GStreamer is working: `gst-inspect-1.0 --version`

### 📦 Package removal issues
- Check for running processes: `ps aux | grep yolov8n`
- Force stop if needed: `pkill -9 mulitistream_yolov8n`
- Then retry removal: `dpkg -r ara2-vision-examples`

## 📄 Licensing

This repository is licensed under the [LA_OPT_Online Code Hosting NXP_Software_License](./LICENSE.txt) license.
