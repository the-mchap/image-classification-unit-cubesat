# SBC ICU - Image Classification Unit

This project implements an Image Classification Unit (ICU) for a Raspberry Pi Zero 2W.

## Prerequisites

The project depends on `libcamera` for camera operations. On Raspberry Pi OS, these dependencies must be installed at the system level.

### System Dependencies

Run the following commands to install the required system packages:

```bash
sudo apt update
sudo apt install -y libcamera-dev libcamera-apps-lite python3-libcamera python3-kms++
```

## Setup

### System Permissions

Ensure your user has permission to access the UART and SPI interfaces:

```bash
sudo usermod -a -G dialout $USER
sudo usermod -a -G spi $USER
sudo usermod -a -G video $USER
```

*Note: You may need to log out and back in for group changes to take effect.*

### Hardware Configuration

Ensure SPI and the Camera are enabled in `raspi-config`:

1. Run `sudo raspi-config`
2. Go to `Interface Options`
3. Enable `SPI`, `I2C`, and `Legacy Camera Support` (if applicable, though `libcamera` is preferred).
4. For `libcamera` on newer OS, ensure `camera_auto_detect=1` is in `/boot/config.txt`.

### Virtual Environment

If you are using a virtual environment (recommended), ensure it can access the system's `libcamera` bindings. There are two ways to do this:

#### Option 1: Recreate venv with system-site-packages (Recommended)

```bash
python3 -m venv --system-site-packages .virtual
source .virtual/bin/activate
pip install -r requirements.txt
```

#### Option 2: Symlink system bindings into existing venv

If you already have a venv and don't want to recreate it:

```bash
# For Python 3.11 (Bookworm)
ln -s /usr/lib/python3/dist-packages/libcamera* .virtual/lib/python3.11/site-packages/
```

## Running the Application

```bash
source .virtual/bin/activate
python3 main.py
```

## Architecture

### Design Decisions
*   **Dual-Stream Capture:** `Picamera2` provides simultaneous high-res JPEG (main) and 224x224 RGB (lores) streams. The hardware ISP handles JPEG encoding and resizing — no software resize or `cv2.imencode`.
*   **In-Memory EXIF:** Custom C++ metadata injector injects EXIF directly into JPEG buffers in RAM. No temp files, no `exiftool` subprocess.
*   **Table-Driven CRC16:** C++ implementation with Python `ctypes` bridge (~2300x faster than pure Python for 4KB blocks).
*   **Zero-Copy Pipeline:** `memoryview` and `ctypes` pointers throughout to minimize RAM copies within 512MB.
*   **SQLite Manifest:** ACID-safe image tracking via `manifest.db`, resilient to power cuts.
*   **Thread-Safe SPI:** `threading.Lock` on all `FlashMemory` operations for concurrent main-thread telemetry + background writes. `WREN`/`PROGRAM` pairs are atomic.
*   **Layered Watchdog:** App → systemd (`sd_notify`, 30s timeout) → hardware watchdog (`/dev/watchdog`).
*   **Non-Blocking Orchestration:** `orchestrator.py` is a state machine that monitors background SPI Flash writes while remaining responsive to UART commands.

### Completed Optimizations
*   Removed `opencv-python` dependency (~150MB savings, faster boot).
*   Eliminated all `subprocess` calls (`exiftool`) and temp files from the image pipeline.
*   Disabled swap (`dphys-swapfile`) to prevent SD card wear.
*   Purged non-essential system services (`avahi-daemon`, `bluez`, `cups`, `alsa-utils`, `ModemManager`, etc.) for RAM/CPU headroom.
*   Rotating log files (`RotatingFileHandler`) to prevent SD card exhaustion.
*   Camera `reset()` for hardware recovery from sensor hangs or electrical noise in orbit.


### Hybrid Integration Strategy (Python/C++)

To sustain performance on the Pi Zero 2W, critical path bottlenecks are refactored into C++ using the following pattern:

*   **Identify Bottleneck:** Use benchmark scripts in `benchmarks/` to quantify Python overhead. 
*   **C++ Implementation:** Implement the logic in `modules/src/` using standard C++ (C-compatible interface with `-extern "C"`). 
*   **Shared Library:** Compile using the root `Makefile` into `.so` files. 
*   **Ctypes Wrapper:** Use `ctypes` in the corresponding Python module with a robust `try-except` fallback to pure Python logic. 
*   **Zero-Copy Principles:** Pass pointers and use memory-mapped techniques to avoid expensive RAM copies between Python and C. 

## Flight Deployment

### 1. Enable Hardware Watchdog
Edit `/etc/systemd/system.conf`, uncomment and set `RuntimeWatchdogSec=15s`.

### 2. Deploy ICU Service
```bash
sudo cp docs/icu_sbc.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now icu_sbc.service
```

### 3. Final OS Cleanup (Flight Only)
Once wireless debugging is no longer needed, disable the radio stack:
```bash
sudo systemctl disable --now NetworkManager wpa_supplicant
```
