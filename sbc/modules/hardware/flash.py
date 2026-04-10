"""SPI NOR Flash driver. 4-byte addressing with non-blocking asynchronous page-program writes."""

import spidev
import time
import threading
from typing import Optional, List, Tuple
from .. import config

logger = config.logging.get_logger(__name__)

__all__ = ["FlashMemory", "FlashMemoryError"]


class FlashMemoryError(Exception):
    """Custom exception for flash memory errors."""

    pass


class FlashMemory:
    """
    Interface for SPI NOR Flash Memory communication (MT25QL01GBBB).
    4-byte address mode and asynchronous writes.
    """

    # ────── Flash Memory Commands (W25Q01JV) ──────
    CMD_WRITE_ENABLE = 0x06
    CMD_WRITE_DISABLE = 0x04
    CMD_READ_STATUS_REG1 = 0x05
    CMD_PAGE_PROGRAM_4B = 0x12  # Page Program with 4-Byte Address

    # ──────  Status Register ──────
    STATUS_WIP_BIT = 0x01  # Write In Progress bit

    # ──────  Flash Memory Layout ──────
    PAGE_SIZE = 256

    # ──────  Timing ──────
    WIP_INTERVAL = 0.00002  # 20us poll interval for WIP bit.

    def __init__(
        self,
        bus: int = config.hardware.SPI_BUS,
        device: int = config.hardware.SPI_DEVICE,
        max_speed_hz: int = config.hardware.SPI_SPEED_HZ,
        mode: int = 0,
    ):
        """
        Initialize the SPI connection to flash memory.
        """
        self.spi: Optional[spidev.SpiDev] = None
        self.is_open = False
        self.bus = bus
        self.device = device
        self._lock = threading.Lock()

        # --- Asynchronous Write State ---
        self._write_thread: Optional[threading.Thread] = None
        self.write_in_progress: bool = False
        self.progress_bytes: int = 0
        self.write_successful: Optional[bool] = None
        self.last_write_error: Optional[str] = None
        self._state_lock = threading.Lock()
        # ---

        try:
            self.spi = spidev.SpiDev()
            self.spi.open(bus, device)
            self.is_open = True
            self.spi.max_speed_hz = max_speed_hz
            self.spi.mode = mode

            logger.info(
                f"SPI connection opened on bus {bus}, device {device} "
                f"(Mode: {self.spi.mode}, Speed: {self.spi.max_speed_hz / 1e6:.1f} MHz)"
            )
        except FileNotFoundError:
            error_msg = (
                f"SPI bus {bus} or device {device} not found. " "Ensure SPI is enabled."
            )
            logger.error(error_msg)
            raise FlashMemoryError(error_msg)
        except Exception as e:
            error_msg = f"Unexpected error during SPI setup: {e}"
            logger.error(error_msg)
            raise FlashMemoryError(error_msg)

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        self.close()

    def close(self) -> None:
        """Closes the SPI connection safely."""
        if self.spi and self.is_open:
            # Wait if a write is still in progress
            with self._state_lock:
                write_in_progress = self.write_in_progress
                write_thread = self._write_thread
            if write_in_progress and write_thread:
                logger.warning("Waiting for SPI write to finish before closing...")
                write_thread.join()

            self.spi.close()
            self.is_open = False
            logger.info("SPI connection closed")

    def _check_connection(self) -> None:
        if not self.is_open:
            raise FlashMemoryError("SPI connection is not open")

    def _xfer(self, command: List[int]) -> List[int]:
        """Thread-safe SPI transfer wrapper."""
        with self._lock:
            return self.spi.xfer2(command)

    def _wait_for_write_complete(self, timeout: float = 10.0) -> bool:
        """Polls the Status Register until the WIP (Write In Progress) bit is 0."""
        self._check_connection()
        start_time = time.time()

        while True:
            # Poll status register (thread-safe)
            status = self._xfer([self.CMD_READ_STATUS_REG1, 0x00])[1]
            if (status & self.STATUS_WIP_BIT) == 0:
                return True
            if time.time() - start_time > timeout:
                raise FlashMemoryError(
                    f"Write operation timeout after {timeout} seconds"
                )
            time.sleep(self.WIP_INTERVAL)

    def _write_enable(self) -> None:
        """Sends the Write Enable (WREN) command."""
        self._xfer([self.CMD_WRITE_ENABLE])

    def _write_page(self, address: int, data_chunk: List[int]) -> None:
        """Writes a single page (up to 256 bytes) to flash memory."""
        self._check_connection()
        if not data_chunk:
            return
        if len(data_chunk) > self.PAGE_SIZE:
            raise FlashMemoryError(f"Chunk size {len(data_chunk)} exceeds page size")

        # Atomic WREN + Program
        with self._lock:
            self.spi.xfer2([self.CMD_WRITE_ENABLE])
            addr_bytes = self._address_to_bytes(address)

            # I use 0x12 for Page Program with 4-byte-address mode, para mas placer.
            command = [self.CMD_PAGE_PROGRAM_4B] + addr_bytes + data_chunk
            self.spi.xfer2(command)

        self._wait_for_write_complete()

    # ────── Asynchronous Write Functions ──────

    def _write_worker(self, address: int, data: memoryview) -> None:
        """Thread worker for SPI writing. Optimized with alignment, bulk, and leftover phases."""
        data_len = len(data)
        with self._state_lock:
            self.progress_bytes = 0
            self.write_successful = None
            self.last_write_error = None

        try:
            logger.info(f"[SPI Worker] Writing {data_len} bytes at 0x{address:08X}")

            # ────── Alignment pass
            # Handle the first (potentially partial) page to align to a physical boundary
            page_offset = address % self.PAGE_SIZE

            if page_offset != 0:
                first_chunk_size = min(data_len, self.PAGE_SIZE - page_offset)
                chunk = data[:first_chunk_size].tolist()
                self._write_page(address, chunk)
                with self._state_lock:
                    self.progress_bytes = first_chunk_size

            # ────── Bulk pass
            # All subsequent writes are guaranteed to be page-aligned physically.
            while (data_len - self.progress_bytes) >= self.PAGE_SIZE:
                curr_addr = address + self.progress_bytes

                chunk = data[
                    self.progress_bytes : self.progress_bytes + self.PAGE_SIZE
                ].tolist()
                self._write_page(curr_addr, chunk)
                with self._state_lock:
                    self.progress_bytes += self.PAGE_SIZE

            # ────── Leftover pass
            # Handle a final partial page
            if self.progress_bytes < data_len:
                curr_addr = address + self.progress_bytes
                chunk = data[self.progress_bytes :].tolist()
                self._write_page(curr_addr, chunk)
                with self._state_lock:
                    self.progress_bytes = data_len

            logger.info(f"[SPI Worker] Completed write of {data_len} bytes.")
            with self._state_lock:
                self.write_successful = True

        except Exception as e:
            logger.error(f"[SPI Worker] Error at {self.progress_bytes} bytes: {e}")
            with self._state_lock:
                self.write_successful = False
                self.last_write_error = str(e)
        finally:
            with self._state_lock:
                self.write_in_progress = False

    def write_bytes_async(
        self, address: int, data: bytes
    ) -> Tuple[bool, Optional[bytes]]:
        """
        Starts writing data to flash in a separate thread (non-blocking).
        Returns (success, end_address_bytes).
        """
        self._check_connection()

        if not data:
            logger.warning("No data to write")
            return False, None

        with self._state_lock:
            if self.write_in_progress:
                logger.error("SPI write already in progress. Request ignored.")
                return False, None

        logger.debug(
            f"Starting SPI write thread for {len(data)} bytes at 0x{address:08X}"
        )

        with self._state_lock:
            self.write_in_progress = True
            self.progress_bytes = 0
            self.write_successful = None
            self.last_write_error = None

        mv = memoryview(data)  # Zero-copy memoryview to avoid massive list allocation

        self._write_thread = threading.Thread(
            target=self._write_worker,
            args=(address, mv),
            daemon=True,
            name="SPIWriteWorker",
        )
        self._write_thread.start()

        end_address = address + len(data)  # End address, last written byte + 1

        return True, end_address.to_bytes(4, "big")

    @property
    def is_write_complete(self) -> bool:
        """Checks if the asynchronous write operation has finished."""
        with self._state_lock:
            return not self.write_in_progress

    @property
    def has_write_failed(self) -> bool:
        """Returns True if the last completed async write failed."""
        with self._state_lock:
            return self.write_successful is False

    @property
    def has_write_succeeded(self) -> bool:
        """Returns True if the last completed async write succeeded."""
        with self._state_lock:
            return self.write_successful is True

    @property
    def write_error(self) -> Optional[str]:
        """Returns the last async write error message, if any."""
        with self._state_lock:
            return self.last_write_error

    @staticmethod
    def _address_to_bytes(address: int) -> List[int]:
        """Converts a 32-bit integer address to a 4-byte list (big endian)."""
        return list(address.to_bytes(4, "big"))
