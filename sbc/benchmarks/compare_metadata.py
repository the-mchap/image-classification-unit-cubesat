import time
import sys
import os
import cv2
import numpy as np
from unittest.mock import MagicMock

# Mock hardware dependencies
sys.modules["spidev"] = MagicMock()
sys.modules["RPi"] = MagicMock()
sys.modules["RPi.GPIO"] = MagicMock()
sys.modules["picamera2"] = MagicMock()
sys.modules["serial"] = MagicMock()
sys.modules["tflite_runtime"] = MagicMock()
sys.modules["tflite_runtime.interpreter"] = MagicMock()

# Add the project root to the path
sys.path.append(os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))

from modules import metadata_handler

def benchmark_metadata_comparison():
    software_tag = "ICU_TEST_v1"
    classification = "LAND"
    capture_time = "2026-02-10 12:00:00"
    model_path = "model_v3.tflite"
    iterations = 50

    # Create a 12MP image
    img = np.zeros((3040, 4056, 3), dtype=np.uint8)
    _, jpeg_buffer = cv2.imencode(".jpg", img)
    image_bytes = jpeg_buffer.tobytes()

    print(f"Comparing Metadata Injection ({iterations} iterations, 12MP Image)...")
    print(f"{'Method':<15} | {'Total Time (s)':<15} | {'Avg Time (ms)':<15}")
    print("-" * 50)

    # 1. Force Pure Python (piexif)
    original_lib = metadata_handler._lib
    metadata_handler._lib = None
    
    start_py = time.time()
    for _ in range(iterations):
        metadata_handler.append_metadata(
            image_bytes, classification, capture_time, model_path, software_tag
        )
    end_py = time.time()
    py_total = end_py - start_py
    py_avg = (py_total / iterations) * 1000
    print(f"{'Pure Python':<15} | {py_total:<15.4f} | {py_avg:<15.4f}")

    # 2. Use C++ Extension
    metadata_handler._lib = original_lib
    if original_lib is None:
        print("C++ Extension NOT AVAILABLE")
        return

    start_cpp = time.time()
    for _ in range(iterations):
        metadata_handler.append_metadata(
            image_bytes, classification, capture_time, model_path, software_tag
        )
    end_cpp = time.time()
    cpp_total = end_cpp - start_cpp
    cpp_avg = (cpp_total / iterations) * 1000
    print(f"{'C++ Extension':<15} | {cpp_total:<15.4f} | {cpp_avg:<15.4f}")
    
    speedup = py_avg / cpp_avg if cpp_avg > 0 else 0
    print("-" * 50)
    print(f"Speedup: {speedup:.2f}x")

if __name__ == "__main__":
    benchmark_metadata_comparison()
