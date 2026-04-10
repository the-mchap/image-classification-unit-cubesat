import time
import cv2
import numpy as np
import io

def profile_jpeg_encoding():
    # 12MP Image
    width, height = 4056, 3040
    img = np.zeros((height, width, 3), dtype=np.uint8)
    # Add some noise to make encoding work a bit
    cv2.randn(img, (128, 128, 128), (50, 50, 50))

    iterations = 20
    print(f"Profiling JPEG Encoding ({iterations} iterations, 12MP Image)...")
    print(f"{'Method':<25} | {'Avg Time (ms)':<15}")
    print("-" * 50)

    # 1. Software Encoding (OpenCV)
    start_sw = time.time()
    for _ in range(iterations):
        _, buf = cv2.imencode(".jpg", img, [cv2.IMWRITE_JPEG_QUALITY, 90])
        _ = buf.tobytes()
    end_sw = time.time()
    sw_avg = ((end_sw - start_sw) / iterations) * 1000
    print(f"{'Software (OpenCV)':<25} | {sw_avg:<15.2f}")

    # 2. Hardware Encoding (Estimation based on Pi Zero 2W specs/docs)
    # Note: On a Pi Zero 2W, hardware encoding is done by the ISP 
    # during capture and is significantly faster than software.
    # Typical values on Pi Zero 2W for 12MP: ~150-300ms 
    # vs Software (OpenCV on Pi Zero 2W): ~1500-3000ms
    # This script runs on the host, but highlights the difference.
    print(f"{'Hardware (estimated)':<25} | {'~200-300'}")

if __name__ == "__main__":
    profile_jpeg_encoding()
