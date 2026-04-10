"""
Image processing pipeline for ICU.
Coordinates capture, classification, and metadata injection.
"""

from typing import Optional, Tuple

from .. import config
from ..processing import metadata
from ..hardware.camera import Camera
from ..processing.neural_network import NeuralNetwork

logger = config.logging.get_logger(__name__)


def add_metadata_to_image(image_bytes: bytes, metadata_string: str) -> bytes:
    """
    Adds compact metadata to the image in memory.
    """
    try:
        image_bytes_with_metadata = metadata.append_metadata(
            image_bytes=image_bytes,
            metadata_string=metadata_string,
            software_tag=config.app.METADATA_SOFTWARE_TAG,
        )
        logger.info(
            f"EXIF metadata added. New size: {len(image_bytes_with_metadata)} bytes."
        )
        return image_bytes_with_metadata
    except Exception as e:
        logger.error(f"Error adding metadata in memory: {e}")
        return image_bytes


def capture_and_process_image(
    camera: Camera, neural_network: NeuralNetwork
) -> Optional[Tuple[bytes, str, float]]:
    """
    Performs one full capture and processing cycle:
    1. Captures dual images (main as JPEG bytes, lores as array).
    2. Classifies the image using the lores image.
    Returns the raw image bytes, classification result, and confidence.
    """
    logger.info("Starting capture and classification cycle (Hardware Optimized)...")
    try:
        # Capture dual images: hi_res is already JPEG bytes
        dual_result = camera.capture_dual()
        if dual_result is None:
            return None
        image_bytes, lo_res_array = dual_result

        # Classify image using the lores array
        classification, confidence = neural_network.predict(lo_res_array)

        # Return the raw image, classification, and confidence
        return image_bytes, classification, confidence

    except Exception as e:
        logger.error(f"Fatal error during capture/process cycle: {e}", exc_info=True)
        return None


def capture_and_classify(
    camera: Camera, neural_network: NeuralNetwork
) -> Optional[Tuple[bytes, str, float]]:
    """
    Runs one capture + classification cycle and validates class labels.
    Returns (image_data, classification, confidence) if valid, else None.
    """
    logger.info("Task: Starting automatic capture and classification.")
    result = capture_and_process_image(camera, neural_network)

    if not result:
        logger.error("Task: Capture failed.")
        return None

    image_data, classification, confidence = result

    if classification in config.app.VALID_IMAGE_CLASSES:
        logger.info(
            "Task: Valid class '%s' (Conf: %.2f, %d bytes).",
            classification,
            confidence,
            len(image_data),
        )
        return image_data, classification, confidence

    logger.info("Task: Invalid class '%s'. Discarding.", classification)
    return None
