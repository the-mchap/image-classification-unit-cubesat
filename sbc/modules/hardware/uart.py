"""UART serial interface manager. Threaded receive and synchronous transmit over /dev/ttyAMA0."""

import serial
import threading
import queue
import logging
from typing import Optional

logger = logging.getLogger(__name__)


class UARTError(Exception):
    """Custom exception for UART communication errors."""

    pass


class UARTManager:
    """Threaded UART manager with background receive and synchronous send.

    Uses a daemon thread to read incoming bytes into an internal queue,
    while send operations are protected by a lock for thread safety.
    """

    def __init__(self, port: str, uart_mcu_speed: int, timeout: float = 1.0):
        """
        Initialize the UART Manager with serial port configuration.

        Args:
            port: The device path for the serial port (e.g., '/dev/ttyS0').
            uart_mcu_speed: Communication speed in bits per second.
            timeout: Maximum time in seconds to wait for a read operation.
        """
        self.port = port
        self.uart_mcu_speed = uart_mcu_speed
        self.timeout = timeout
        self._serial_port: Optional[serial.Serial] = None
        self._listener_thread: Optional[threading.Thread] = None
        self._data_queue: queue.Queue = queue.Queue()
        self._is_running: bool = False
        self._lock = threading.Lock()
        self._send_lock = threading.Lock()

    def __enter__(self):
        """
        Context manager entry point. Returns the UARTManager instance.
        """
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        """
        Context manager exit point. Ensures the serial port and listener thread are stopped.
        """
        self.stop()

    def start(self) -> None:
        """
        Open the serial port and start the background listener thread.

        Raises:
            UARTError: If the listener is already running or if the serial port cannot be opened.
        """
        with self._lock:
            if self._is_running:
                raise UARTError(
                    "Listener is already running. Stop it before starting again."
                )

            try:
                self._serial_port = serial.Serial(
                    self.port, self.uart_mcu_speed, timeout=self.timeout
                )
                logger.info(
                    f"Serial port {self.port} opened at {self.uart_mcu_speed} baud (timeout: {self.timeout}s)"
                )
            except serial.SerialException as e:
                error_msg = f"Could not open serial port {self.port}: {e}"
                logger.error(error_msg)
                raise UARTError(error_msg)

            self.clear_queue()

            self._is_running = True
            self._listener_thread = threading.Thread(
                target=self._serial_listener, daemon=True, name="SerialListener"
            )
            self._listener_thread.start()
            logger.info("UART listener started successfully.")

    def stop(self) -> None:
        """
        Stop the listener thread and close the serial port.
        Ensures a clean shutdown by joining the thread and releasing hardware resources.
        """
        with self._lock:
            if not self._is_running and not self._serial_port:
                logger.debug("Listener already stopped.")
                return

            logger.info("Stopping serial listener...")
            self._is_running = False

            if self._listener_thread and self._listener_thread.is_alive():
                self._listener_thread.join(timeout=2.0)

            if self._serial_port and self._serial_port.is_open:
                try:
                    self._serial_port.close()
                    logger.info("Serial port closed.")
                except Exception as e:
                    logger.error(f"Error closing serial port: {e}")

            self._serial_port = None
            self._listener_thread = None

    def _serial_listener(self) -> None:
        """
        Internal worker thread that continuously monitors the serial port for incoming data.
        Data is read in chunks and pushed to an internal thread-safe queue.
        """
        logger.info("Listener thread started.")
        while self._is_running:
            try:
                if not self._is_port_open():
                    break
                incoming_data = self._read_available_bytes()
                if incoming_data:
                    self._enqueue_data(incoming_data)
            except serial.SerialException as e:
                if self._is_running:
                    logger.error(f"Serial exception in listener: {e}")
                break
            except Exception as e:
                if self._is_running:
                    logger.error(f"Unexpected error in listener: {e}")
                break
        logger.info("Listener thread finished.")

    def send(self, data: bytes) -> bool:
        """
        Synchronously send a byte sequence over the UART interface.
        Uses a dedicated send lock to ensure thread safety during transmission.

        Args:
            data: The bytes to be transmitted.

        Returns:
            True if all bytes were successfully written, False otherwise.

        Raises:
            UARTError: If the serial port is not initialized or closed.
        """
        with self._send_lock:
            if not self._serial_port or not self._serial_port.is_open:
                raise UARTError("Cannot send data, serial port is not open.")
            try:
                total_written = 0
                view = memoryview(data)

                while total_written < len(data):
                    bytes_written = self._serial_port.write(view[total_written:])
                    if bytes_written <= 0:
                        logger.error(
                            "Serial write returned %d while sending frame: %s",
                            bytes_written,
                            data.hex(),
                        )
                        return False
                    total_written += bytes_written

                # Ensure all data is flushed to hardware
                self._serial_port.flush()
                logger.debug(f"Sent {total_written} bytes: {data.hex()}")
                return True

            except serial.SerialException as e:
                logger.error(f"Error sending data: {e}")
                return False

    def get_data(self, timeout: Optional[float] = None) -> Optional[bytes]:
        """
        Retrieve the next chunk of received bytes from the internal queue.

        Args:
            timeout: Optional maximum time in seconds to wait for data.
                     If None, the operation is non-blocking.

        Returns:
            The received bytes if available, or None if the queue is empty or timeout expires.
        """
        try:
            if timeout is None:
                return self._data_queue.get_nowait()
            else:
                return self._data_queue.get(timeout=timeout)
        except queue.Empty:
            return None

    def clear_queue(self) -> int:
        """
        Drain all pending data from the receive queue.

        Returns:
            The number of items (byte chunks) removed from the queue.
        """
        count = 0
        while not self._data_queue.empty():
            try:
                self._data_queue.get_nowait()
                count += 1
            except queue.Empty:
                break
        if count > 0:
            logger.debug(f"Cleared {count} items from receive queue.")
        return count

    # --- Helpers for _serial_listener ---
    def _is_port_open(self) -> bool:
        """
        Validate the current state of the serial port.

        Returns:
            True if the port is initialized and open, False otherwise.
            If False and the manager is supposed to be running, it triggers a shutdown.
        """
        if self._serial_port and self._serial_port.is_open:
            return True
        if self._is_running:
            logger.error("Serial port is not open. Stopping listener.")
            self._is_running = False
        return False

    def _read_available_bytes(self) -> bytes:
        """
        Read all currently available bytes from the serial buffer.
        Blocks until at least one byte arrives or the timeout occurs.

        Returns:
            A bytes object containing the raw data received.
        """
        assert self._serial_port is not None
        bytes_to_read = max(1, self._serial_port.in_waiting)
        return self._serial_port.read(bytes_to_read)

    def _enqueue_data(self, data: bytes) -> None:
        """
        Thread-safe insertion of received data into the internal queue.

        Args:
            data: The byte sequence to store.
        """
        self._data_queue.put(data)
        logger.debug(f"Received {len(data)} bytes: {data.hex()}")

    # ---

    @property
    def is_running(self) -> bool:
        """
        Check if the UART listener is currently active.
        """
        return self._is_running

    @property
    def queue_size(self) -> int:
        """
        Return the approximate number of data chunks currently in the receive queue.
        """
        return self._data_queue.qsize()
