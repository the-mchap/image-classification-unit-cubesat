"""
UART Protocol Module for ICU.
Handles frame construction, parsing, and validation (CRC-16).
"""

import queue
import time
from enum import Enum
from typing import Optional

from . import crc
from ..config import logging as app_logging
from ..hardware.uart import UARTManager

logger = app_logging.get_logger(__name__)

__all__ = [
    "HEADER",
    "STOP_BYTE",
    "RxCommand",
    "TxCommand",
    "TxProtocol",
    "RxProtocol",
    "NACK_CHECKSUM_FRAME",
    "NACK_INVALID_FRAME",
]

# =========================
# Protocol Constants
# =========================
HEADER = 0x3E
STOP_BYTE = 0x0A
INVALID_FIELD = 0xFF
NULL_FIELD = 0x00

# Specific Frames
NACK_CHECKSUM_FRAME = bytes(
    [HEADER, INVALID_FIELD, NULL_FIELD, INVALID_FIELD, INVALID_FIELD, STOP_BYTE]
)
NACK_INVALID_FRAME = bytes(
    [HEADER, NULL_FIELD, NULL_FIELD, NULL_FIELD, NULL_FIELD, STOP_BYTE]
)

# Acknowledgment configuration
ACK_TIMEOUT_SECONDS = 0.15  # 150ms timeout for MCU ACK at 9600 baud


class BufferSize:
    MAX = 45
    MIN = 6


class FrameIndex:
    HEADER = 0
    COMMAND = 1
    PAYLOAD_LEN = 2
    PAYLOAD_START = 3


class FrameOffset:
    CRC_MSB = 3
    CRC_LSB = 4
    OVERHEAD = 6


class FrameType(Enum):
    COMMAND = 1
    ACK = 2
    NACK_FRAME = 3
    NACK_CRC = 4


# ICU -> RPi
class RxCommand(Enum):
    WRITE_FROM = 0x30
    ABORT_WRITE = 0x31
    CAM_ON = 0x32
    POWEROFF = 0x33
    REBOOT = 0x34
    REPORT = 0x35
    THUMBNAIL_CODE = 0x36
    FULL_CODE = 0x37


# RPi -> ICU
class TxCommand(Enum):
    WRITE_REQ = 0x40
    WRITE_BEG = 0x41
    WRITE_END = 0x42
    BOOT_READY = 0x43
    PWROFF_ACK = 0x44
    DB_REPORT = 0x45


# Default priorities for TxCommands (lower is higher priority)
TX_PRIORITY = {
    TxCommand.WRITE_REQ: 10,
    TxCommand.WRITE_BEG: 5,  # Higher priority to ensure write flow
    TxCommand.WRITE_END: 5,
    TxCommand.PWROFF_ACK: 0,  # Max priority
    TxCommand.BOOT_READY: 20,
    TxCommand.DB_REPORT: 30,  # Telemetry can wait
}


class TxProtocol:
    """Handles construction and transmission of protocol frames."""

    def __init__(
        self, uart_manager: UARTManager, frame_index: type[FrameIndex] = FrameIndex
    ):
        """
        Initialize the transmission protocol handler.

        Args:
            uart_manager: Instance of UARTManager for physical layer access.
            frame_index: Protocol index mapping class.
        """
        self.uart = uart_manager
        self.frame_idx = frame_index
        self.last_tx_signature: Optional[bytes] = None
        self.last_tx_timestamp: float = 0.0
        self._pending_commands: queue.PriorityQueue = queue.PriorityQueue()
        self._counter = 0  # To ensure stable sorting for same priority

    def send_command(
        self,
        command: TxCommand,
        payload: bytes = b"",
        enqueue: bool = False,
        priority: Optional[int] = None,
    ) -> bool:
        """
        Construct and transmit a command frame.
        Stores the frame signature to identify the expected mirrored ACK.

        Args:
            command: The TxCommand enumeration member to send.
            payload: Optional byte sequence to include as frame payload.
            enqueue: If True, queue the command for later transmission.
            priority: Optional manual priority override.

        Returns:
            True if command was queued or transmission was successful, False otherwise.
        """
        if enqueue:
            if priority is None:
                priority = TX_PRIORITY.get(command, 50)

            # Store (priority, counter, command, payload) to the priority queue
            self._pending_commands.put((priority, self._counter, command, payload))
            self._counter += 1

            logger.debug(
                "Queued TX %s command (prio=%d, payload_len=%d). Pending=%d",
                command.name,
                priority,
                len(payload),
                self.pending_commands,
            )
            return True

        return self._send_command_now(command, payload)

    def _send_command_now(self, command: TxCommand, payload: bytes = b"") -> bool:
        """
        Construct and transmit a command frame immediately.
        """
        frame = self._build_command_frame(command, payload)
        self.last_tx_signature = self._get_ack_signature(frame)
        self.last_tx_timestamp = time.time()

        if command in (TxCommand.WRITE_REQ, TxCommand.WRITE_BEG, TxCommand.WRITE_END):
            logger.info("TX %s frame: %s", command.name, frame.hex())

        logger.debug(
            f"Sending frame: {frame.hex()} (sig: {self.last_tx_signature.hex()})"
        )
        return self.uart.send(frame)

    def flush_queued_commands(self, limit: int = 1) -> int:
        """
        Send queued commands based on priority.
        Will not send if waiting for an ACK, unless it has timed out.

        Args:
            limit: Maximum number of queued commands to send in this call.

        Returns:
            Number of successfully transmitted queued commands.
        """
        # Check if we are waiting for an ACK
        if self.last_tx_signature is not None:
            if time.time() - self.last_tx_timestamp < ACK_TIMEOUT_SECONDS:
                return 0  # Still waiting for ACK, skip flush

            logger.warning("ACK timeout for last TX. Clearing signature and retrying.")
            self.clear_pending_ack

        sent = 0
        while sent < limit:
            try:
                # PriorityQueue returns (priority, counter, command, payload)
                _, _, command, payload = self._pending_commands.get_nowait()
            except queue.Empty:
                break

            if not self._send_command_now(command, payload):
                logger.error("Queued TX %s failed.", command.name)
                # Note: We don't re-queue here to avoid infinite loops on hardware failure
                break

            sent += 1
            # We break after one send if it requires an ACK,
            # ensuring next flush waits for the ACK.
            if self.last_tx_signature is not None:
                break

        return sent

    @property
    def pending_commands(self) -> int:
        """Approximate count of queued commands waiting for transmission."""
        return self._pending_commands.qsize()

    def send_ack(self, received_frame: bytes) -> bool:
        """
        Respond to a received command with a mirrored ACK frame.
        Mirrors the Command ID and CRC of the received frame.

        Args:
            received_frame: The raw bytes of the command frame being acknowledged.

        Returns:
            True if ACK transmission was successful.
        """
        ack = self._build_mirrored_ack(received_frame)
        logger.info("TX ACK frame (%d bytes): %s", len(ack), ack.hex())
        sent = self.uart.send(ack)
        if not sent:
            logger.error("ACK transmission failed for frame: %s", ack.hex())
        return sent

    @property
    def send_crc_nack(self) -> bool:
        """
        Transmit a Negative Acknowledgment (NACK) specifically for CRC failures.

        Returns:
            True if NACK transmission was successful.
        """
        return self.uart.send(NACK_CHECKSUM_FRAME)

    @property
    def send_frame_nack(self) -> bool:
        """
        Transmit a Negative Acknowledgment (NACK) for malformed or invalid frames.

        Returns:
            True if NACK transmission was successful.
        """
        return self.uart.send(NACK_INVALID_FRAME)

    @property
    def clear_pending_ack(self) -> None:
        """
        Reset the stored transmission signature once a matching ACK is received.
        """
        self.last_tx_signature = None
        self.last_tx_timestamp = 0.0

    @staticmethod
    def _build_command_frame(command: TxCommand, payload: bytes = b"") -> bytes:
        """
        Assemble a complete protocol frame for transmission.
        Format: [HEADER, CMD, PAYLOAD_LEN, PAYLOAD..., CRC_MSB, CRC_LSB, STOP_BYTE]

        Args:
            command: The command ID.
            payload: The payload bytes.

        Returns:
            The fully assembled byte sequence.
        """
        payload_len = len(payload)
        frame = bytearray([HEADER, command.value, payload_len])
        frame.extend(payload)

        # CRC is calculated over [CMD, LEN, PAYLOAD]
        crc_val = crc.generate_for_send(bytes(frame[1:]))

        frame.append((crc_val >> 8) & 0xFF)
        frame.append(crc_val & 0xFF)
        frame.append(STOP_BYTE)
        return bytes(frame)

    def _get_ack_signature(self, frame: bytes) -> bytes:
        """
        Extract the identifying signature from a transmitted frame.
        The signature is used to match incoming mirrored ACKs.

        Args:
            frame: The transmitted frame bytes.

        Returns:
            A 3-byte signature: [Command, CRC_MSB, CRC_LSB].
        """
        payload_len = frame[self.frame_idx.PAYLOAD_LEN]
        crc_start = self.frame_idx.PAYLOAD_START + payload_len
        # Command byte + 2 CRC bytes
        return bytes(
            [frame[self.frame_idx.COMMAND], frame[crc_start], frame[crc_start + 1]]
        )

    def _build_mirrored_ack(self, received_frame: bytes) -> bytes:
        """
        Create an ACK frame by mirroring the command and CRC from a received frame.
        Format: [HEADER, CMD, 0x00, CRC_MSB, CRC_LSB, STOP_BYTE]

        Args:
            received_frame: The command frame to acknowledge.

        Returns:
            The assembled ACK frame bytes.
        """
        crc_start = (
            self.frame_idx.PAYLOAD_START + received_frame[self.frame_idx.PAYLOAD_LEN]
        )

        return bytes(
            [
                HEADER,
                received_frame[self.frame_idx.COMMAND],
                0x00,  # NULL_FIELD
                received_frame[crc_start],
                received_frame[crc_start + 1],
                0x0A,  # STOP_BYTE
            ]
        )


class RxProtocol:
    """Handles reception, buffering, and parsing of protocol frames."""

    def __init__(
        self,
        uart_manager: UARTManager,
        tx_protocol: TxProtocol,
        buffer_size: type[BufferSize] = BufferSize,
        frame_index: type[FrameIndex] = FrameIndex,
        frame_offset: type[FrameOffset] = FrameOffset,
    ):
        """
        Initialize the reception protocol handler.

        Args:
            uart_manager: Instance of UARTManager for physical layer access.
            tx_protocol: Instance of TxProtocol for sending ACKs/NACKs.
            buffer_size: Protocol buffer limits class.
            frame_index: Protocol index mapping class.
            frame_offset: Protocol CRC offset mapping class.
        """
        self.uart = uart_manager
        self.tx = tx_protocol
        self.buff_size = buffer_size
        self.idx = frame_index
        self.offset = frame_offset
        self.buffer = bytearray()

    def get_next_command(self) -> Optional[bytes]:
        """
        Identify and extract the next valid command frame from the receive buffer.
        Performs buffer alignment, length validation, and CRC checks.
        Automatically manages protocol state (ACKs, NACKs, TX signature clearing).

        Returns:
            The raw bytes of a valid command frame if found, else None.
        """
        self._pull_data()

        if not self._is_valid_frame():
            return None

        frame = self._extract_frame()
        frame_type = self._get_frame_type(frame)

        if frame_type == FrameType.COMMAND:
            return self._get_if_valid(frame)

        elif frame_type == FrameType.ACK:
            logger.debug("Received ACK for sent command: %s", frame.hex())
            self.tx.clear_pending_ack

        elif frame_type == FrameType.NACK_CRC:
            logger.error("Received NACK CRC from MCU: %s", frame.hex())

        else:
            logger.error("Received NACK FRAME from MCU: %s", frame.hex())

        return None

    def get_command(self, frame: bytes) -> RxCommand:
        """
        Extract the Command ID from a valid protocol frame.

        Args:
            frame: A validated protocol frame.

        Returns:
            The enum value of the command byte.
        """
        return RxCommand(frame[self.idx.COMMAND])

    def get_payload(self, frame: bytes) -> Optional[bytes]:
        """
        Extract the payload portion from a protocol frame.

        Args:
            frame: A validated protocol frame.

        Returns:
            The payload bytes if payload length > 0, else None.
        """
        payload_len = frame[self.idx.PAYLOAD_LEN]

        if payload_len == 0:
            return None

        end = self.idx.PAYLOAD_START + payload_len
        return frame[self.idx.PAYLOAD_START : end]

    def _pull_data(self) -> None:
        """
        Transfer all pending data from the UARTManager queue to the internal processing buffer.
        Enforces a maximum buffer size to prevent memory exhaustion on malformed streams.
        """
        while True:
            data = self.uart.get_data()
            if data is None:
                break
            self.buffer.extend(data)

        # Keep buffer inside protocol limit:
        if len(self.buffer) > self.buff_size.MAX:
            logger.warning("Buffer overflow, clearing old data.")
            self.buffer = self.buffer[-self.buff_size.MAX :]

    def _is_valid_frame(self) -> bool:
        """
        Verify if the current buffer contains a structurally valid protocol frame.
        Checks for header alignment, minimum length, valid payload length, and stop byte.

        Returns:
            True if a complete frame is ready for extraction, False otherwise.
        """
        if not self._align_buffer_to_header():
            return False

        if len(self.buffer) < self.buff_size.MIN:
            return False

        payload_len = self.buffer[self.idx.PAYLOAD_LEN]

        if not self._is_payload_length_valid(payload_len):
            del self.buffer[0]  # Pop bad header, let loop realign
            return False

        expected_len = payload_len + self.offset.OVERHEAD
        if expected_len > len(self.buffer):
            return False  # Frame is valid so far, but incomplete. Wait for more bytes.

        if not self._is_stop_byte_valid(expected_len):
            del self.buffer[0]  # Pop bad header, let loop realign
            return False

        return True

    def _align_buffer_to_header(self) -> bool:
        """
        Locate the first occurrence of the HEADER byte and discard preceding garbage bytes.

        Returns:
            True if the buffer starts with a HEADER, False if no HEADER is present.
        """
        header_idx = self.buffer.find(HEADER)

        if header_idx > 0:
            logger.debug(f"Discarding {header_idx} bytes of leading garbage.")
            del self.buffer[:header_idx]
        elif header_idx == -1:
            self.buffer.clear()
            return False

        return len(self.buffer) > 0

    def _is_payload_length_valid(self, payload_len: int) -> bool:
        """
        Validate that the declared payload length is within protocol constraints.

        Args:
            payload_len: Length byte from the frame.

        Returns:
            True if the length is within the allowed maximum.
        """
        max_allowed_payload = self.buff_size.MAX - self.offset.OVERHEAD
        if payload_len > max_allowed_payload:
            logger.warning(
                f"Payload length {payload_len} exceeds max {max_allowed_payload}. Discarding header."
            )
            return False
        return True

    def _is_stop_byte_valid(self, expected_len: int) -> bool:
        """
        Check if the STOP_BYTE is present at the expected frame boundary.

        Args:
            expected_len: The calculated total length of the frame.

        Returns:
            True if the byte at expected_len - 1 matches STOP_BYTE.
        """
        if self.buffer[expected_len - 1] != STOP_BYTE:
            logger.warning(
                f"Frame missing STOP_BYTE at index {expected_len - 1}. Discarding header."
            )
            return False
        return True

    def _extract_frame(self) -> bytes:
        """
        Slice the validated frame from the buffer and remove it from the internal stream.

        Returns:
            The raw bytes of the extracted frame.
        """
        expected_len = self.buffer[self.idx.PAYLOAD_LEN] + self.offset.OVERHEAD
        frame = bytes(self.buffer[:expected_len])
        del self.buffer[:expected_len]

        return frame

    def _get_frame_type(self, frame: bytes) -> FrameType:
        """
        Classify a structurally valid frame as a Command, ACK, or NACK.

        Args:
            frame: The frame bytes to classify.

        Returns:
            The classified FrameType member.
        """
        if len(frame) > self.buff_size.MIN:  # It can only be a command.
            return FrameType.COMMAND

        if frame == NACK_CHECKSUM_FRAME:
            return FrameType.NACK_CRC
        if frame == NACK_INVALID_FRAME:
            return FrameType.NACK_FRAME

        if self._is_ack(frame):  # Check if it's an ACK to last sent command
            return FrameType.ACK

        return FrameType.COMMAND

    def _get_if_valid(self, frame: bytes) -> Optional[bytes]:
        """
        Perform a final CRC check on a command frame.
        Automatically sends an ACK on success or a NACK on failure.

        Args:
            frame: The command frame to validate.

        Returns:
            The frame bytes if CRC is valid, else None.
        """
        if self._has_valid_crc(frame):
            logger.debug(f"Received valid command frame: {frame.hex()}")
            if not self.tx.send_ack(frame):
                logger.error("Failed to send ACK for valid command: %s", frame.hex())
            return frame

        logger.warning(f"CRC mismatch for command frame: {frame.hex()}")
        self.tx.send_crc_nack
        return None

    def _is_ack(self, frame: bytes) -> bool:
        """
        Determine if a 6-byte frame matches the signature of the last transmitted command.

        Args:
            frame: The 6-byte frame (mirrored ACK candidate).

        Returns:
            True if the signature matches.
        """
        return self.tx.last_tx_signature == bytes(
            [
                frame[self.idx.COMMAND],
                frame[self.offset.CRC_MSB],
                frame[self.offset.CRC_LSB],
            ]
        )

    @staticmethod
    def _has_valid_crc(frame: bytes) -> bool:
        """
        Verify the integrity of a frame using CRC-16.

        Args:
            frame: The frame to check.

        Returns:
            True if the calculated CRC matches the one embedded in the frame.
        """
        return crc.calculate_for_received(frame) == crc.extract_checksum(frame)
