"""
Thumbnail module for ICU.
Handles in-memory image compression and thumbnail generation for downlink.
"""

import io
import os
from typing import Optional
from PIL import Image
from .. import config

logger = config.logging.get_logger(__name__)

__all__ = ["generate_miniature_bytes"]

# Default thumbnail dimensions and quality for "light" re-confirmation
THUMBNAIL_SIZE = (640, 480)
THUMBNAIL_QUALITY = 35

# Default quality for "compressed full" downlink
FULL_DL_QUALITY = 70


def generate_miniature_bytes(
    image_path: str, is_thumbnail: bool = True
) -> Optional[bytes]:
    """
    Reads an image from disk and compresses it for downlink in RAM.
    """
    if not os.path.exists(image_path):
        logger.error(f"Thumbnail: Source image {image_path} not found.")
        return None

    try:
        with Image.open(image_path) as img:
            quality = _prepare_image(img, is_thumbnail)
            return _compress_to_bytes(img, quality)
    except Exception as e:
        logger.error(f"Thumbnail: Failed to process {image_path}: {e}", exc_info=True)
        return None


def _prepare_image(img: Image.Image, is_thumbnail: bool) -> int:
    """Resizes the image if needed and returns the target compression quality."""
    if is_thumbnail:
        # Resize while maintaining aspect ratio (LANCZOS for high quality downscaling)
        img.thumbnail(THUMBNAIL_SIZE, Image.Resampling.LANCZOS)
        logger.info(f"Thumbnail: Generating {THUMBNAIL_SIZE} preview.")
        return THUMBNAIL_QUALITY

    logger.info(f"Thumbnail: Re-compressing full image (Quality: {FULL_DL_QUALITY}).")
    return FULL_DL_QUALITY


def _compress_to_bytes(img: Image.Image, quality: int) -> bytes:
    """Saves the image into a memory buffer and returns the raw JPEG bytes."""
    output = io.BytesIO()
    # optimize=True performs an extra pass to reduce file size further
    img.save(output, format="JPEG", quality=quality, optimize=True)

    compressed_bytes = output.getvalue()
    logger.info(
        f"Thumbnail: Compression complete. Size: {len(compressed_bytes)} bytes."
    )
    return compressed_bytes
