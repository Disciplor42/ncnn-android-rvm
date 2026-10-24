#!/usr/bin/env python3
"""
FlipCam PC Receiver
Connects to TCP 127.0.0.1:9000 (ADB forwarded), decodes H.264 stream,
and writes YUV420P frames to /dev/video99 (v4l2loopback).
"""

import sys
import socket
import struct
import time
import os
import av
import numpy as np

HOST = "127.0.0.1"
PORT = 9000
V4L2_DEVICE = "/dev/video99"

def recv_exact(sock, n):
    buf = bytearray()
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            return None
        buf.extend(chunk)
    return bytes(buf)

def main():
    print("==========================================")
    print("        FlipCam PC Receiver Starting       ")
    print("==========================================")
    print(f"Target Server: {HOST}:{PORT}")
    print(f"Virtual Camera: {V4L2_DEVICE}")

    if not os.path.exists(V4L2_DEVICE):
        print(f"ERROR: Virtual camera device '{V4L2_DEVICE}' not found.")
        print("Please load v4l2loopback module via setup.sh first:")
        print("  sudo modprobe v4l2loopback devices=1 video_nr=99 card_label='FlipCam' exclusive_caps=1")
        sys.exit(1)

    try:
        v4l_fd = os.open(V4L2_DEVICE, os.O_RDWR)
    except Exception as e:
        print(f"ERROR: Unable to open {V4L2_DEVICE}: {e}")
        sys.exit(1)

    codec = av.CodecContext.create("h264", "r")

    while True:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

        print(f"\n[*] Connecting to {HOST}:{PORT}...")
        while True:
            try:
                sock.connect((HOST, PORT))
                print("[+] Connected to FlipCam phone stream!")
                break
            except (ConnectionRefusedError, OSError):
                print(".", end="", flush=True)
                time.sleep(1)

        frame_count = 0
        start_time = time.time()

        try:
            while True:
                len_bytes = recv_exact(sock, 4)
                if not len_bytes:
                    print("\n[-] Connection closed by phone.")
                    break

                frame_len = struct.unpack(">I", len_bytes)[0]
                packet_data = recv_exact(sock, frame_len)
                if not packet_data:
                    print("\n[-] Connection dropped while receiving packet.")
                    break

                packet = av.Packet(packet_data)
                try:
                    frames = codec.decode(packet)
                    for frame in frames:
                        yuv_frame = frame.to_ndarray(format="yuv420p")
                        os.write(v4l_fd, yuv_frame.tobytes())

                        frame_count += 1
                        if frame_count % 30 == 0:
                            elapsed = time.time() - start_time
                            fps = frame_count / elapsed if elapsed > 0 else 0
                            print(f"[+] Receiving and streaming: {frame.width}x{frame.height} @ {fps:.1f} FPS", end="\r", flush=True)
                except Exception:
                    pass

        except (socket.error, KeyboardInterrupt) as e:
            if isinstance(e, KeyboardInterrupt):
                print("\n[!] Exiting receiver...")
                os.close(v4l_fd)
                sock.close()
                sys.exit(0)
            print(f"\n[-] Socket error: {e}")
        finally:
            sock.close()
            print("[*] Reconnecting in 1 second...")
            time.sleep(1)

if __name__ == "__main__":
    main()
