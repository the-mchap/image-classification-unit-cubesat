"""Application-level constants: image classes, camera dimensions, storage paths, and timing."""

import os
from . import loader

""" --- Memory Sections ---"""
# Data Section boundaries
DATA_1ST = 0x00002000
DATA_END = 0x07FFFFFF

""" --- Image Classes ---"""
# Valid classes to be saved
VALID_IMAGE_CLASSES = ["LAND", "SKY", "WATER"]
# Mapping for compact metadata (3 characters)
CLASS_MAP = {"LAND": "LND", "SKY": "COU", "WATER": "WTR"}
# Invalid classes to be discarded
INVALID_IMAGE_CLASS = "BAD"

""" --- Camera and ML Configuration ---"""
# Get the absolute path of the directory containing this file (app.py)
_CURRENT_DIR = os.path.dirname(os.path.abspath(__file__))
# Get the project root directory (which is two levels up from 'modules/config')
_PROJECT_ROOT = os.path.dirname(os.path.dirname(_CURRENT_DIR))

# Path to the TFLite model
MODEL_PATH = os.path.join(_PROJECT_ROOT, "models", "model_v3.tflite")
# Quality of the JPEG image to be saved in Flash
IMAGE_QUALITY = loader.get_int("camera", "image_quality", fallback=85)
# Storage paths
STORAGE_DIR = os.path.join(_PROJECT_ROOT, "storage")
DATABASE_PATH = os.path.join(STORAGE_DIR, "manifest.db")
# Create storage dir if it doesn't exist
if not os.path.exists(STORAGE_DIR):
    os.makedirs(STORAGE_DIR, exist_ok=True)

# Dimensions of the captured image (for saving to flash)
IMAGE_WIDTH = loader.get_int("camera", "capture_width", fallback=1280)
IMAGE_HEIGHT = loader.get_int("camera", "capture_height", fallback=960)
# Dimensions of the inference image (for TFLite)
TFLITE_WIDTH = 224
TFLITE_HEIGHT = 224
# Tag for EXIF
METADATA_SOFTWARE_TAG = "RPi-Zero-2W-v1.0"
# Unit conversion
MILLISECONDS_TO_SECONDS = 0.001

# Orchestrator loop idle sleep (conf value is in ms, converted to seconds)
_DEFAULT_LOOP_INTERVAL_MS = 10
LOOP_INTERVAL = loader.get_int("orchestrator", "loop_interval", fallback=_DEFAULT_LOOP_INTERVAL_MS) * MILLISECONDS_TO_SECONDS
# Watchdog interval (seconds)
WATCHDOG_INTERVAL = loader.get_int("watchdog", "watchdog_interval", fallback=10)

# Benchmarks
HARDWARE_SAMPLING = loader.get_bool("benchmarks", "hardware_sampling", fallback=False)
SAMPLING_INTERVAL = loader.get_int("benchmarks", "sampling_interval", fallback=1)
SAMPLER_SCRIPT = os.path.join(_PROJECT_ROOT, "benchmarks", "raw_hardware_sampler.sh")
SAMPLER_OUTPUT = os.path.join(_PROJECT_ROOT, "benchmarks", "hardware_benchmark.csv")
