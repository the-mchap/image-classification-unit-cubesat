import time
import random
import sys
import os
from unittest.mock import MagicMock

# Mock hardware dependencies for non-Pi environments
sys.modules["spidev"] = MagicMock()
sys.modules["RPi"] = MagicMock()
sys.modules["RPi.GPIO"] = MagicMock()
sys.modules["picamera2"] = MagicMock()
sys.modules["serial"] = MagicMock()
sys.modules["tflite_runtime"] = MagicMock()
sys.modules["tflite_runtime.interpreter"] = MagicMock()
sys.modules["piexif"] = MagicMock()
sys.modules["cv2"] = MagicMock()

# Add the project root to the path so we can import modules
sys.path.append(os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))

from modules import crc_16
from modules.config import protocol


def generate_random_data(size):
    return bytes([random.randint(0, 255) for _ in range(size)])


def benchmark():
    # Test cases: Small (Command), Medium (Telemetry), Large (Image Chunk)
    sizes = [10, 256, 1024, 4096]
    iterations = 1000

    print(
        f"{'Size (Bytes)':<15} | {'Iterations':<12} | {'Total Time (s)':<15} | {'Avg Time (ms)':<15} | {'Throughput (MB/s)':<15}"
    )
    print("-" * 85)

    for size in sizes:
        data = generate_random_data(size)
        start_time = time.time()

        for _ in range(iterations):
            # We use calculate_bitwise directly to avoid the packet structure parsing overhead
            # and strictly measure the CRC algorithm performance.
            crc_16.calculate_bitwise(data)

        end_time = time.time()
        total_time = end_time - start_time
        avg_time_ms = (total_time / iterations) * 1000
        throughput = (size * iterations / (1024 * 1024)) / total_time

        print(
            f"{size:<15} | {iterations:<12} | {total_time:<15.4f} | {avg_time_ms:<15.4f} | {throughput:<15.2f}"
        )


if __name__ == "__main__":
    print("Benchmarking Pure Python CRC16 Implementation...")
    benchmark()
