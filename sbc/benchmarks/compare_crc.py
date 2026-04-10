import time
import random
import sys
import os
import ctypes
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

# Load the C++ shared library directly for benchmarking
lib_path = os.path.abspath(
    os.path.join(os.path.dirname(__file__), "../modules/src/libcrc16.so")
)
lib = ctypes.CDLL(lib_path)
lib.calculate_crc16.argtypes = [ctypes.POINTER(ctypes.c_uint8), ctypes.c_size_t]
lib.calculate_crc16.restype = ctypes.c_uint16


def calculate_crc_cpp(data):
    data_p = (ctypes.c_uint8 * len(data)).from_buffer_copy(data)
    return lib.calculate_crc16(data_p, len(data))


def calculate_crc_python_bitwise(data_to_process: bytes) -> int:
    crc = 0x1D0F
    polynomial = 0x1021
    top_bit = 0x8000
    for byte in data_to_process:
        crc ^= byte << 8
        for _ in range(8):
            if crc & top_bit:
                crc = (crc << 1) ^ polynomial
            else:
                crc = crc << 1
    return crc & 0xFFFF


def generate_random_data(size):
    return bytes([random.randint(0, 255) for _ in range(size)])


def benchmark():
    sizes = [10, 256, 1024, 4096, 16384]
    iterations = 1000

    print(
        f"{'Size (Bytes)':<15} | {'Type':<10} | {'Total Time (s)':<15} | {'Avg Time (ms)':<15} | {'Throughput (MB/s)':<15}"
    )
    print("-" * 85)

    for size in sizes:
        data = generate_random_data(size)

        # Python bitwise benchmark
        start_time = time.time()
        for _ in range(iterations):
            calculate_crc_python_bitwise(data)
        py_total_time = time.time() - start_time
        py_avg_time_ms = (py_total_time / iterations) * 1000
        py_throughput = (size * iterations / (1024 * 1024)) / py_total_time
        print(
            f"{size:<15} | {'Py Bitwise':<10} | {py_total_time:<15.4f} | {py_avg_time_ms:<15.4f} | {py_throughput:<15.2f}"
        )

        # C++ table benchmark
        start_time = time.time()
        for _ in range(iterations):
            calculate_crc_cpp(data)
        cpp_total_time = time.time() - start_time
        cpp_avg_time_ms = (cpp_total_time / iterations) * 1000
        cpp_throughput = (size * iterations / (1024 * 1024)) / cpp_total_time
        print(
            f"{size:<15} | {'C++ Table':<10} | {cpp_total_time:<15.4f} | {cpp_avg_time_ms:<15.4f} | {cpp_throughput:<15.2f}"
        )
        print("-" * 85)


if __name__ == "__main__":
    print("Comparing Pure Python Bitwise vs C++ Table-Driven CRC16 Implementation...")
    benchmark()
