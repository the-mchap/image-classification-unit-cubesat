"""
Task Handler module for ICU.
Processes specific commands received over UART.
"""

import os
import time
from typing import Optional, Tuple

from .. import config
from . import pipeline, system
from ..processing import thumbnail
from ..hardware.camera import Camera
from ..hardware.flash import FlashMemory
from ..processing.neural_network import NeuralNetwork
from ..processing.protocol import TxProtocol, TxCommand
from ..processing.database import DataBase

logger = config.logging.get_logger(__name__)

__all__ = [
    "capture_mission",
    "send_db_report",
    "handle_abort_write",
    "handle_image_request",
    "handle_image_on_flash",
    "handle_power_off",
    "handle_reboot",
]

# Global to store selected image metadata across UART commands
_selected_image_id: Optional[int] = None
_selected_image_filename: Optional[str] = None
_selected_is_thumbnail: bool = False


# Category codes used in UART payloads
class CategoryCode:
    SKY = 0x43
    WATER = 0x57
    LAND = 0x4C


CATEGORY_MAP = {
    CategoryCode.SKY: "SKY",
    CategoryCode.WATER: "WATER",
    CategoryCode.LAND: "LAND",
}


# Image request criteria used in UART payloads
class ImageRequestCriterion:
    METADATA_CODE = 0x50
    LATEST = 0x52
    HIGHEST_CONFIDENCE = 0x54


# Capture mission iteration limits
CAPTURE_MISSION_LIMIT_MIN = 1
CAPTURE_MISSION_LIMIT_MAX = 50

# Count limit per classification
CLASSIFICATION_COUNT_MAX = 255

# Binary payload and numeric conversion sizes
IMAGE_REQ_MIN_PAYLOAD_LEN = 2
IMAGE_REQ_METADATA_PAYLOAD_LEN = 3
U32_BYTE_LEN = 4
WRITE_END_PAYLOAD_LEN = 8

# Capture mission timing and confidence scaling
CAPTURE_LOOP_DELAY_SECONDS = 0.5
CONFIDENCE_PERCENT_SCALE = 100


def capture_mission(
    tx: TxProtocol,
    storage: DataBase,
    camera: Camera,
    neural_network: NeuralNetwork,
    limit: int = 1,
) -> None:
    """
    Executes the main routine sequence in a loop.
    1. Captures dual images and classifies.
    2. Injects metadata and stores to MicroSD/Database.
    3. Loop continues until `limit` is reached.
    """
    limit = _sanitize_capture_limit(limit)
    logger.info(f"Task Handler: Starting automatic capture sequence (limit: {limit}).")

    for i in range(limit):
        system.watchdog_ping()
        _run_capture_cycle(storage, camera, neural_network, i + 1, limit)
        time.sleep(CAPTURE_LOOP_DELAY_SECONDS)

    send_db_report(tx, storage)


def send_db_report(tx: TxProtocol, storage: DataBase) -> bool:
    """
    Queries the database for unsent image counts and sends a DB_REPORT.
    Format: [count][C][count][W][count][L] (6 bytes)
    Counts capped at 255 (0xFF).
    """
    counts = storage.get_unsent_counts()

    # Cap
    sky_count = min(counts.get("SKY", 0), CLASSIFICATION_COUNT_MAX)
    water_count = min(counts.get("WATER", 0), CLASSIFICATION_COUNT_MAX)
    land_count = min(counts.get("LAND", 0), CLASSIFICATION_COUNT_MAX)

    # Encode: [count][C][count][W][count][L] -> C = 0x43, W = 0x57, L = 0x4C
    payload = bytes(
        [
            sky_count,
            CategoryCode.SKY,
            water_count,
            CategoryCode.WATER,
            land_count,
            CategoryCode.LAND,
        ]
    )

    logger.info(
        "Task Handler: Sending DB_REPORT. Counts: SKY=%d, WATER=%d, LAND=%d. Payload: %s",
        sky_count,
        water_count,
        land_count,
        payload.hex(),
    )

    return tx.send_command(TxCommand.DB_REPORT, payload, enqueue=True)


def handle_abort_write(tx: TxProtocol) -> bool:
    """
    Clears the temporal image selection without affecting the database.
    """
    logger.info("Task Handler: Aborting write. Clearing temporal selection.")
    _clear_selection()
    return True


def handle_image_request(
    tx: TxProtocol,
    storage: DataBase,
    payload: Optional[bytes],
    is_thumbnail: bool = False,
) -> bool:
    """
    Handles a request for an image (full or thumbnail) based on criteria.
    1. Resolves request payload to a specific image.
    2. Calculates size (thumbnail or full).
    3. Sends WRITE_REQ with size to the MCU.
    """
    image_result, request_label = _resolve_requested_image(storage, payload)

    if request_label is None or not image_result:
        if request_label:
            logger.warning("Task Handler: No image found for %s.", request_label)
        return False

    image_id, filename = image_result
    file_path = os.path.join(config.app.STORAGE_DIR, filename)

    selected_size = _get_image_downlink_size(file_path, is_thumbnail)
    if selected_size is None:
        _clear_selection()
        return False

    # Store selection info for the upcoming flash write
    global _selected_image_id, _selected_image_filename, _selected_is_thumbnail
    _selected_image_id = image_id
    _selected_image_filename = filename
    _selected_is_thumbnail = is_thumbnail

    # Notify MCU of the pending write size
    size_bytes = selected_size.to_bytes(U32_BYTE_LEN, "big")
    logger.info(
        f"Task Handler: Prepared {filename} ({selected_size} bytes). Sending WRITE_REQ."
    )

    return tx.send_command(TxCommand.WRITE_REQ, size_bytes, enqueue=True)


def handle_image_on_flash(
    tx: TxProtocol,
    flash: FlashMemory,
    storage: DataBase,
    address_bytes: Optional[bytes],
) -> Optional[bytes]:
    """
    Starts an asynchronous flash write for the currently selected image.
    1. Validates selection and target address.
    2. Builds binary data and triggers async write.
    3. Returns the WRITE_END payload to be sent once write finishes.
    """
    logger.info("Task Handler: Received WRITE_FROM command.")

    if _selected_image_filename is None:
        logger.error("Task Handler: No image selected for write.")
        return None

    address = int.from_bytes(address_bytes or b"", byteorder="big", signed=False)
    if not _is_address_valid(address):
        logger.error("Task Handler: Flash address 0x%08X is out of range.", address)
        return None

    file_path = os.path.join(config.app.STORAGE_DIR, _selected_image_filename)
    try:
        data = _build_image_downlink_data(file_path, _selected_is_thumbnail)
    except Exception as e:
        logger.error("Task Handler: Data build failed: %s", e)
        return None

    if not data:
        return None

    success, end_address_bytes = flash.write_bytes_async(address, data)
    if not success or end_address_bytes is None:
        logger.error("Task Handler: Flash write failed to start.")
        return None

    # Mark as sent and prepare the final telemetry payload
    if _selected_image_id is not None:
        storage.mark_as_sent(_selected_image_id)

    payload = _format_write_end_payload(end_address_bytes, len(data))
    if payload:
        _clear_selection()
        tx.send_command(TxCommand.WRITE_BEG, enqueue=True)
        logger.info("Task Handler: Flash write sequence initiated at 0x%08X.", address)

    return payload


def handle_power_off(tx: TxProtocol) -> bool:
    """Executes the system power-off."""
    logger.info("Task Handler: Received POWEROFF command.")
    tx.send_command(TxCommand.PWROFF_ACK, enqueue=True)
    system.shutdown("poweroff")
    return False


def handle_reboot(tx: TxProtocol) -> bool:
    """Executes the system reboot."""
    logger.info("Task Handler: Received REBOOT command.")
    tx.send_command(TxCommand.PWROFF_ACK, enqueue=True)
    system.shutdown("reboot")
    return False


def _clear_selection() -> None:
    """Resets the global image selection state."""
    global _selected_image_id, _selected_image_filename, _selected_is_thumbnail
    _selected_image_id = None
    _selected_image_filename = None
    _selected_is_thumbnail = False


def _sanitize_capture_limit(limit: int) -> int:
    """Ensures the capture limit stays within allowed safety bounds."""
    if limit > CAPTURE_MISSION_LIMIT_MAX:
        logger.warning(
            "Task Handler: Limit %d exceeds max. Capping at %d.",
            limit,
            CAPTURE_MISSION_LIMIT_MAX,
        )
        return CAPTURE_MISSION_LIMIT_MAX
    if limit < CAPTURE_MISSION_LIMIT_MIN:
        logger.warning(
            "Task Handler: Limit %d below min. Setting to %d.",
            limit,
            CAPTURE_MISSION_LIMIT_MIN,
        )
        return CAPTURE_MISSION_LIMIT_MIN
    return limit


def _run_capture_cycle(
    storage: DataBase,
    camera: Camera,
    neural_network: NeuralNetwork,
    index: int,
    total: int,
) -> None:
    """Executes a single capture, classification, and storage iteration."""
    logger.debug(f"Task Handler: Automatic capture {index}/{total}.")
    result = pipeline.capture_and_classify(camera, neural_network)

    if not result:
        logger.warning("Task Handler: Capture failed or invalid. Skipping.")
        return

    image_data, classification, confidence = result

    # Prepare metadata: [COUNT][CLASS][CONF]
    count = storage.count_by_classification(classification) + 1
    class_code = config.app.CLASS_MAP.get(classification, "ERR")
    conf_hex = f"{int(confidence * CONFIDENCE_PERCENT_SCALE):02X}"
    metadata_str = f"{count:02X}{class_code}{conf_hex}"

    # Inject and Save
    image_data = pipeline.add_metadata_to_image(image_data, metadata_str)
    if storage.save_image(image_data, classification, confidence):
        logger.info(
            f"Task Handler: Image {index} saved ({classification}, conf: {confidence:.2f})."
        )
    else:
        logger.error(f"Task Handler: Failed to save image {index} to storage.")


def _get_image_downlink_size(file_path: str, is_thumbnail: bool) -> Optional[int]:
    """Calculates the byte size of the image to be downlinked (full or thumbnail)."""
    try:
        if is_thumbnail:
            # We must generate the thumbnail to know its exact size
            data = thumbnail.generate_miniature_bytes(file_path, True)
            return len(data) if data else None
        return os.path.getsize(file_path)
    except Exception as e:
        logger.error(f"Task Handler: Failed to calculate downlink size: {e}")
        return None


def _is_address_valid(address: int) -> bool:
    """Verifies if the flash address is within the dedicated data partition."""
    return config.app.DATA_1ST <= address <= config.app.DATA_END


def _format_write_end_payload(
    end_address_bytes: bytes, data_len: int
) -> Optional[bytes]:
    """Combines end address and data size into a standard 8-byte WRITE_END payload."""
    size_bytes = data_len.to_bytes(U32_BYTE_LEN, "big")
    payload = end_address_bytes + size_bytes
    if len(payload) != WRITE_END_PAYLOAD_LEN:
        logger.error("Task Handler: Invalid WRITE_END payload size %d.", len(payload))
        return None
    return payload


def _resolve_requested_image(
    storage: DataBase, payload: Optional[bytes]
) -> Tuple[Optional[Tuple[int, str]], Optional[str]]:
    """
    Resolves an image request payload into a DB image tuple `(image_id, filename)`.
    Returns `(image_result, request_label)`.
    `request_label` is `None` when the payload is invalid/missing.
    """
    if payload is None:
        logger.error("Task Handler: Image request payload is missing.")
        return None, None

    if len(payload) < IMAGE_REQ_MIN_PAYLOAD_LEN:
        logger.error("Task Handler: Image request payload is too short.")
        return None, None

    criterion = payload[0]

    if criterion == ImageRequestCriterion.METADATA_CODE:
        if len(payload) < IMAGE_REQ_METADATA_PAYLOAD_LEN:
            logger.error(
                "Task Handler: 0x%02X request payload is missing category.",
                ImageRequestCriterion.METADATA_CODE,
            )
            return None, None

        count = payload[1]
        category_byte = payload[2]
        classification = CATEGORY_MAP.get(category_byte)
        if not classification:
            logger.error(f"Task Handler: Invalid category byte 0x{category_byte:02X}.")
            return None, None

        logger.info(
            f"Task Handler: Image request (Metadata Code) - Category: {classification}, Index: {count}."
        )
        return storage.get_image_by_index(classification, count), (
            f"{classification} index {count}"
        )

    category_byte = payload[1]
    classification = CATEGORY_MAP.get(category_byte)
    if not classification:
        logger.error(f"Task Handler: Invalid category byte 0x{category_byte:02X}.")
        return None, None

    if criterion == ImageRequestCriterion.LATEST:
        logger.info(
            f"Task Handler: Image request (Latest) - Category: {classification}."
        )
        return storage.get_latest_image(classification), f"latest {classification}"

    if criterion == ImageRequestCriterion.HIGHEST_CONFIDENCE:
        logger.info(
            f"Task Handler: Image request (Highest Confidence) - Category: {classification}."
        )
        return (
            storage.get_highest_confidence_image(classification),
            f"highest-confidence {classification}",
        )

    logger.warning(f"Task Handler: Criterion 0x{criterion:02X} is not implemented.")
    return None, None


def _build_image_downlink_data(file_path: str, is_thumbnail: bool) -> Optional[bytes]:
    """
    Builds bytes to downlink from a stored image path.
    If `is_thumbnail` is True, generates a compressed thumbnail.
    Otherwise, returns original file bytes unchanged.
    """
    if is_thumbnail:
        return thumbnail.generate_miniature_bytes(file_path, True)

    with open(file_path, "rb") as image_file:
        return image_file.read()
