"""
ICU_RPI modules package.
"""

from . import config
from .config.logging import get_logger
from .hardware.flash import FlashMemory, FlashMemoryError
from .hardware.camera import Camera
from .hardware.uart import UARTManager, UARTError
from .processing.database import DataBase
from .processing.neural_network import NeuralNetwork
from .processing.protocol import RxProtocol, TxProtocol, FrameType, RxCommand, TxCommand
from .processing import crc, metadata, thumbnail
from .core import task_handler, orchestrator, system, pipeline

__all__ = [
    "config",
    "get_logger",
    "FlashMemory",
    "FlashMemoryError",
    "DataBase",
    "Camera",
    "NeuralNetwork",
    "UARTManager",
    "UARTError",
    "RxProtocol",
    "TxProtocol",
    "FrameType",
    "RxCommand",
    "TxCommand",
    "crc",
    "metadata",
    "thumbnail",
    "task_handler",
    "orchestrator",
    "system",
    "pipeline",
]
