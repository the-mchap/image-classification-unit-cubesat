"""Hardware interface constants loaded from icu.conf: UART, SPI bus/device/speed."""

from . import loader

UART_PORT = "/dev/ttyAMA0"
UART_SPEED = loader.get_int("hardware", "uart_mcu_speed", fallback=115200)
SPI_BUS = 0
SPI_DEVICE = 1
SPI_SPEED_HZ = loader.get_int("hardware", "data_write_speed", fallback=10) * 1_000_000
