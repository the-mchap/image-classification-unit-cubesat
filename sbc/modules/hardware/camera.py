"""Picamera2 dual-stream driver. Provides simultaneous JPEG capture and low-res inference input."""

import time
import io
import numpy as np
import cv2
from picamera2 import Picamera2
from typing import Optional, Tuple

from .. import config

logger = config.logging.get_logger(__name__)

__all__ = ["Camera"]


class Camera:
    """
    Handle all interactions with the PiCamera, including
    configuration and image capture.
    """

    def __init__(
        self,
        width: int,
        height: int,
        lores_width: int,
        lores_height: int,
    ):
        """
        Initializes and configures the camera with dual streams.
        """
        self.camera = Picamera2()
        self._configure_camera(width, height, lores_width, lores_height)

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        self.close()

    def _configure_camera(
        self, width: int, height: int, lores_width: int, lores_height: int
    ):
        self.main_size = (width, height)
        self.lores_size = (lores_width, lores_height)
        try:
            # On Pi Zero 2W, YUV420 is the most compatible format for multiple
            # hardware-scaled streams. request.save(format="jpeg") can encode
            # this YUV420 buffer to a JPEG file using hardware/simplejpeg.
            cam_config = self.camera.create_video_configuration(
                main={"size": self.main_size, "format": "YUV420"},
                lores={"size": self.lores_size, "format": "YUV420"},
            )
            self.camera.configure(cam_config)
            self.camera.start()
            time.sleep(1)
            logger.info("HQ Camera ready: Main (YUV420) + Lores (YUV420)")
        except Exception as e:
            logger.critical(f"Camera configuration failed: {e}")
            raise e

    def reset(self):
        """
        Safely restarts the camera hardware. Use this if capture fails or
        sensor becomes unresponsive.
        """
        logger.warning("Resetting camera hardware...")
        try:
            self.camera.stop()
            time.sleep(0.5)
            self.camera.start()
            logger.info("Camera hardware reset successful.")
        except Exception as e:
            logger.error(f"Failed to reset camera: {e}")
            # Re-initialize the whole thing if stop/start fails
            try:
                self.camera.close()
                self.camera = Picamera2()
                self._configure_camera(
                    self.main_size[0],
                    self.main_size[1],
                    self.lores_size[0],
                    self.lores_size[1],
                )
            except Exception as e2:
                logger.critical(f"Complete camera re-initialization failed: {e2}")

    def capture_dual(self) -> Optional[Tuple[bytes, np.ndarray]]:
        request = None
        try:
            # capture_request() blocks until a frame is captured and returns
            # a CompletedRequest object containing the synchronized buffers.
            request = self.camera.capture_request()

            # 1. Main Stream -> JPEG (Hardware Encoded via MJPEG format)
            # We use request.save() to extract exactly the encoded JPEG bytes
            # from the MJPEG buffer. We specify format="jpeg" because jpeg_io
            # (BytesIO) doesn't have a file extension for auto-detection.
            jpeg_io = io.BytesIO()
            request.save("main", jpeg_io, format="jpeg", quality=config.app.IMAGE_QUALITY)
            hi_res_bytes = jpeg_io.getvalue()

            # 2. Lores Stream -> YUV -> RGB
            # request.make_array() returns the YUV420 planar buffer as a numpy array.
            # Note: This array might have a larger stride (e.g., 256 instead of 224)
            # due to ISP alignment requirements on the Pi Zero 2W.
            yuv_array = request.make_array("lores")

            # Convert YUV420 to RGB for the neural network
            lo_res_rgb = cv2.cvtColor(yuv_array, cv2.COLOR_YUV2RGB_I420)

            # Crop the image to the actual requested dimensions to remove ISP padding.
            # This ensures the dimensions are exactly (224, 224, 3) for the ML model.
            lo_res_rgb = lo_res_rgb[: self.lores_size[1], : self.lores_size[0]]

            request.release()
            request = None

            return hi_res_bytes, lo_res_rgb

        except Exception as e:
            logger.error(f"Hardware capture failed: {e}", exc_info=True)
            return None
        finally:
            if request is not None:
                request.release()

    def close(self):
        """Stops and closes the camera safely."""
        try:
            if self.camera:
                self.camera.stop()
                self.camera.close()
                logger.info("Camera stopped and closed.")
        except Exception as e:
            logger.error(f"Error closing the camera: {e}")
