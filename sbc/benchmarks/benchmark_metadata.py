import time
import sys
import os
import io
import cv2
import numpy as np
from unittest.mock import MagicMock

# Mock hardware dependencies for non-Pi environments
sys.modules["spidev"] = MagicMock()
sys.modules["RPi"] = MagicMock()
sys.modules["RPi.GPIO"] = MagicMock()
sys.modules["picamera2"] = MagicMock()
sys.modules["serial"] = MagicMock()
sys.modules["tflite_runtime"] = MagicMock()
sys.modules["tflite_runtime.interpreter"] = MagicMock()

# Add the project root to the path so we can import modules
sys.path.append(os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))

from modules import metadata_handler


def benchmark():
    software_tag = "ICU_TEST_v1"

    # Create a 12MP JPEG using cv2
    img = np.zeros((3040, 4056, 3), dtype=np.uint8)
    _, jpeg_buffer = cv2.imencode(".jpg", img)
    image_bytes = jpeg_buffer.tobytes()

    iterations = 20
    classification = "LAND"
    capture_time = "2026-02-10 12:00:00"
    model_path = "model_v3.tflite"

    print(f"Benchmarking Metadata Injection ({iterations} iterations)...")
    start_time = time.time()
    for _ in range(iterations):
        metadata_handler.append_metadata(
            image_bytes, classification, capture_time, model_path, software_tag
        )
    end_time = time.time()

    total_time = end_time - start_time
    avg_time_ms = (total_time / iterations) * 1000

    print(f"Total Time: {total_time:.4f} s")
    print(f"Avg Time per image: {avg_time_ms:.4f} ms")


if __name__ == "__main__":
    benchmark()
