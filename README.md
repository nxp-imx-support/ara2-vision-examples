<div align="center">

# NXP Ara240 Vision Examples

[![License](https://img.shields.io/badge/License-Proprietary-red)](./LICENSE.txt)
[![Platforms](https://img.shields.io/badge/Platforms-FRDM_i.MX_8M_Plus_|_FRDM_i.MX_95|_FRDM_i.MX_95_PRO-blue)](https://www.nxp.com/products/processors-and-microcontrollers/arm-processors/i-mx-applications-processors:IMX_HOME)
[![Language](https://img.shields.io/badge/C++-00599C?logo=cplusplus)](https://isocpp.org/)
[![AI/ML](https://img.shields.io/badge/AI/ML-Vision-orange)](https://www.nxp.com/docs/en/user-guide/UG10166.pdf)
[![BSP](https://img.shields.io/badge/BSP_>=-LF6.18.20--2.0.0-purple.svg?logo=linux&logoColor=white)](https://www.nxp.com/design/design-center/software/embedded-software/i-mx-software/embedded-linux-for-i-mx-applications-processors:IMXLINUX)

---

</div>

## 📖 Project Description

This repository contains comprehensive vision examples for i.MX
processors with Ara240 DNPU acceleration. It demonstrates
real-time video processing with AI/ML inference capabilities
including object detection, classification, pose estimation, and
semantic segmentation.

## 💻 Supported Platforms

| Platform            | Supported |
| ------------------- | :-------: |
| [FRDM i.MX 8M Plus] |    ✅     |
| [FRDM i.MX 95]      |    ✅     |
| FRDM i.MX 95 PRO    |    ✅     |

[FRDM i.MX 8M Plus]: (https://www.nxp.com/design/design-center/development-boards-and-designs/FRDM-IMX8MPLUS)
[FRDM i.MX 95]: (https://www.nxp.com/design/design-center/development-boards-and-designs/FRDM-IMX95)

## 📋 Requirements

### 💻 Software

- Ara240 Runtime SDK installed on target
- [Embedded Linux for i.MX](https://www.nxp.com/design/design-center/software/embedded-software/i-mx-software/embedded-linux-for-i-mx-applications-processors:IMXLINUX)
  (>= LF6.18.20_2.0.0)
- ara2-vision-example.deb (optional, build instructions available in this repository)


## 🔨 Build `ara2-vision-examples.deb` package

1.  Clone the repository on your host PC:

```sh
git clone https://github.com/nxp-imx-support/ara2-vision-examples.git
```

2. Change directory to the repository and set the RTSDK path:

```sh
export RTSDK_PATH=<path_to_your_rtsdk>
```

**Example:**

```sh
export RTSDK_PATH=<>/rt-sdk-ara2/rt-sdk-ara2/
```

3. Run the build script. Make sure you have the NXP toolchain
   installed for the FRDM BSP version you need. Steps to build
   the toolchain are available at
   [iMX Linux User's Guide](https://www.nxp.com/docs/en/user-guide/UG10163.pdf)

```bash
bash build.sh <path_to_your_toolchain>
```

## 🧰 Installation on i.MX

**NOTE:** Make sure the Ara240 Runtime SDK is installed in the
FRDM i.MX system before moving forward.

1. Copy the `ara2-vision-examples.deb` to the FRDM i.MX board.
   You can use `scp` command as below if you know the IP of the
   target board:

```sh
scp ara2-vision-examples.deb root@<ip_addr>:
```

2. Install the package with the following command:

```sh
dpkg -i ara2-vision-examples.deb
```

### 🗑️ Uninstalling the Package

To remove the package while keeping configuration files:

```sh
dpkg -r ara2-vision-examples
```

To completely remove the package including all configuration
files:

```sh
dpkg -P ara2-vision-examples
```

### ✅ Verifying Installation

Check if the package is installed:

```sh
dpkg -l | grep ara2-vision-examples
```

View package information:

```sh
dpkg -s ara2-vision-examples
```

List installed files:

```sh
dpkg -L ara2-vision-examples
```

## 🎯 Available examples

| Snapshot | Name | Platforms | Implementation | Model |
| :---: | --- | --- | :---: | :---: |
| <a href="./tasks/object-detection/yolo/multistream-gstreamer/README.md"><img src="./data/yoloExample.webp" width="150" alt="multistream_yolo"></a> | [multistream_yolo](./tasks/object-detection/yolo/multistream-gstreamer/README.md) | FRDM i.MX 8M Plus<br>FRDM i.MX 95<br>FRDM i.MX 95 PRO | C++ | YOLOv8n <br>YOLOv8s <br>YOLOv8m <br>YOLOv8l <br>YOLOv8x <br>YOLOxs <br>YOLOxm <br>YOLOxl |

## 🔧 Troubleshooting

### 🚫 Application won't start

- Ensure `imx-nxp-ara2` is installed:
  `dpkg -l | grep imx-nxp-ara2`
- Check if the model is downloaded:
  `ls /usr/share/cnn/detection`
- Verify GStreamer is working: `gst-inspect-1.0 --version`

### 📦 Package removal issues

- Check for running processes: `ps aux | grep yolov8n`
- Force stop if needed: `pkill -9 mulitistream_yolov8n`
- Then retry removal: `dpkg -r ara2-vision-examples`

## 📄 Licensing

NXP Proprietary. This software is owned or controlled by NXP and
may only be used strictly in accordance with the applicable
license terms. By expressly accepting such terms or by
downloading, installing, activating and/or otherwise using the
software, you are agreeing that you have read, and that you
agree to comply with and are bound by, such license terms. If
you do not agree to be bound by the applicable license terms,
then you may not retain, install, activate or otherwise use the
software.

This repository is licensed under the
[LA_OPT_Online Code Hosting NXP_Software_License](./LICENSE.txt)
license.
