#!/bin/bash
set -e

echo "=========================================="
echo "    FlipCam Linux Virtual Cam Setup       "
echo "=========================================="

# 1. Detect Linux Distro and Install Dependencies
if [ -f /etc/os-release ]; then
    . /etc/os-release
    DISTRO=$ID
else
    DISTRO="unknown"
fi

echo "[*] Detected distro: $DISTRO"

case "$DISTRO" in
    ubuntu|debian|pop|mint)
        echo "[*] Installing dependencies for Debian/Ubuntu..."
        sudo apt-get update
        sudo apt-get install -y python3-av v4l2loopback-dkms adb
        ;;
    fedora)
        echo "[*] Installing dependencies for Fedora..."
        sudo dnf install -y python3-pyav v4l2loopback android-tools
        ;;
    arch|manjaro)
        echo "[*] Installing dependencies for Arch Linux..."
        sudo pacman -S --needed --noconfirm python-av v4l2loopback-dkms android-tools
        ;;
    *)
        echo "[!] Unknown distro. Please manually ensure python3-av, v4l2loopback, and adb are installed."
        ;;
esac

# 2. Check adb binary
if ! command -v adb &> /dev/null; then
    echo "[!] ERROR: 'adb' command not found. Please install Android Platform Tools (adb)."
    exit 1
fi

# 3. Load v4l2loopback Kernel Module
echo "[*] Loading v4l2loopback virtual camera (/dev/video99)..."
sudo modprobe -r v4l2loopback 2>/dev/null || true
sudo modprobe v4l2loopback devices=1 video_nr=99 card_label="FlipCam" exclusive_caps=1

if [ ! -c /dev/video99 ]; then
    echo "[!] ERROR: Failed to create virtual camera device /dev/video99"
    exit 1
fi
echo "[+] Virtual camera /dev/video99 is ready!"

# 4. ADB Port Forwarding
echo "[*] Setting up ADB port forwarding (tcp:9000 -> tcp:9000)..."
if ! adb devices | grep -q "device$"; then
    echo "[!] WARNING: No Android device detected via ADB."
    echo "    Make sure USB Debugging is enabled on your phone and connected via USB."
fi

adb forward tcp:9000 tcp:9000
echo "[+] ADB port forward tcp:9000 configured!"

# 5. Run Python Receiver
echo "[*] Launching FlipCam PC receiver..."
python3 phone_cam_receiver.py
