# FlipCam — On-Phone AI Background Removal → Virtual Webcam on Linux

FlipCam turns your Android smartphone into an AI-powered virtual webcam for Linux using USB ADB port forwarding. It performs on-device AI background removal with Robust Video Matting (RVM) and composites the user onto a magenta background, streaming low-latency H.264 video directly to a `/dev/video99` virtual camera device for OBS Studio.

---

## Requirements

### Phone (Android)
- **Architecture:** `arm64-v8a`
- **Operating System:** Android 9.0 (API 28) or newer
- **RAM:** 4 GB minimum
- **Connection:** USB Debugging enabled

### PC (Linux)
- **Distribution:** Arch Linux, Fedora, or Debian/Ubuntu-based distros
- **Packages:** `adb`, `v4l2loopback-dkms`, `python3-av` (installed automatically by `setup.sh`)
- **Applications:** OBS Studio (or any application supporting V4L2 webcam inputs)

---

## Quick Start

### Step 1: Install APK on Android Phone
Build or download the `flipcam.apk` and install it on your phone:
```bash
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

### Step 2: Connect USB & Run Setup Script
Connect your phone to your PC via USB (ensure USB Debugging is turned on) and run:
```bash
./setup.sh
```
This script automatically installs required packages, loads the `/dev/video99` v4l2loopback virtual camera module, configures `adb forward tcp:9000 tcp:9000`, and launches the Python receiver.

### Step 3: Configure OBS Studio
1. Open OBS Studio.
2. Open your Android app, select your preferred model (Fast / Quality) and resolution (Auto / 1080p / 720p / 540p), and tap **START STREAM**.
3. In OBS, add a new **Video Capture Device (V4L2)** source and choose **FlipCam** (`/dev/video99`).
4. Add a **Chroma Key** filter to the source with key color `#FF00FF` (Magenta).

---

## OBS Configuration Steps

1. In the **Sources** panel, click **+** → **Video Capture Device (V4L2)**.
2. Select `/dev/video99` (labelled **FlipCam**).
3. Right-click the video source → **Filters**.
4. Click **+** under Effect Filters → select **Chroma Key**.
5. Set **Key Color Type** to **Custom**, and enter HTML Color Code `#FF00FF`.
6. Adjust **Similarity** (approx. 0.15) and **Smoothness** (approx. 0.15) until background is completely transparent.
7. Add your desired background image/video layer underneath the camera source in OBS.

*(Placeholder: `![OBS Setup Screenshot](docs/obs_setup.png)`)*

---

## Build from Source

### Prerequisites
- **Android Studio** (2023.1+)
- **Android NDK** (`r26b` or `r27` / NDK `30.0.16248370`)
- **CMake** (`3.22.1` or newer)

### Build Steps
1. Clone the repository:
   ```bash
   git clone https://github.com/nihui/ncnn-android-rvm.git
   cd ncnn-android-rvm
   ```
2. Download pre-built ncnn and opencv-mobile libraries into `app/src/main/jni` as described in `app/src/main/jni/CMakeLists.txt`.
3. Build APK using Gradle:
   ```bash
   ./gradlew assembleDebug
   ```

---

## Troubleshooting

### "adb: command not found"
- Install Android platform-tools via your package manager (`sudo apt install adb`, `sudo dnf install android-tools`, or `sudo pacman -S android-tools`).

### "v4l2loopback not found" or "Failed to create /dev/video99"
- Ensure Linux kernel headers and DKMS are installed for your running kernel.
- Run `sudo modprobe v4l2loopback devices=1 video_nr=99 card_label="FlipCam" exclusive_caps=1`.

### "No frames received"
- Verify ADB detects your phone: `adb devices`.
- Check ADB port forwarding is active: `adb forward --list`. Re-run `adb forward tcp:9000 tcp:9000`.
- Ensure you tapped **START STREAM** inside the phone app.

### "Low FPS / Laggy Video"
- Switch Model selection from **Quality (ResNet50)** to **Fast (MobileNetV3)** in the app UI.
- Lower the resolution selection to **720p** or **540p**.

---

## FAQ

### Why magenta (#FF00FF)?
Magenta is rarely present in clothing, skin tones, or room environments, making it an ideal chroma key target that prevents accidental cutout transparency.

### Why not green?
Green frequently appears in plants, clothing, background elements, and eye colors, leading to unwanted keying artifacts.

### Can I use this without OBS?
Yes! Any Linux application that reads V4L2 webcam devices (such as Zoom, Google Meet, Discord, or WebRTC browsers) can select `/dev/video99`.

### Why USB only?
USB ADB port forwarding guarantees deterministic ultra-low latency (< 120ms) and avoids packet loss or congestion caused by Wi-Fi networks.

---

## License

This project is licensed under the [MIT License](LICENSE).
