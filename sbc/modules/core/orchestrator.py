"""Orchestrator module for ICU. Coordinates services and handles the main event loop."""

import time
from typing import Callable
from . import system
from .. import config
from . import task_handler
from dataclasses import dataclass
from ..processing.protocol import RxProtocol, TxProtocol, RxCommand, TxCommand
from ..hardware.flash import FlashMemory
from ..processing.database import DataBase
from ..hardware.camera import Camera
from ..processing.neural_network import NeuralNetwork
from ..hardware.uart import UARTManager

logger = config.logging.get_logger(__name__)

__all__ = ["Services", "start_event_loop"]

IDLE_SLEEP = config.app.LOOP_INTERVAL


@dataclass(frozen=True)
class Services:
    """Initialized system services container."""

    rx: RxProtocol
    tx: TxProtocol
    flash: FlashMemory
    storage: DataBase
    camera: Camera
    neural_network: NeuralNetwork
    uart: UARTManager


@dataclass
class _LoopState:
    """Internal mutable state for the main event loop."""

    flash_write_in_progress: bool = False
    pending_write_end_payload: bytes = b""
    last_watchdog_ping: float = 0.0
    running: bool = True


CommandHandler = Callable[[Services, bytes, _LoopState], None]


def _build_dispatch_table() -> dict[RxCommand, CommandHandler]:
    return {
        RxCommand.CAM_ON: _handle_cam_on,
        RxCommand.WRITE_FROM: _handle_write_from,
        RxCommand.ABORT_WRITE: _handle_abort_write,
        RxCommand.POWEROFF: _handle_poweroff,
        RxCommand.REBOOT: _handle_reboot,
        RxCommand.REPORT: _handle_report_request,
        RxCommand.THUMBNAIL_CODE: _handle_image_request,
        RxCommand.FULL_CODE: _handle_image_request,
    }


def _dispatch_command(
    services: Services,
    frame: bytes,
    state: _LoopState,
    handlers: dict[RxCommand, CommandHandler],
) -> None:
    command = services.rx.get_command(frame)
    handler = handlers.get(command)
    if handler is None:
        logger.error(f"Orchestrator: Unhandled command {command.name}.")
        return
    handler(services, frame, state)


def start_event_loop(services: Services):
    """
    The main application event loop.
    It listens for UART commands and dispatches them to the task_handler.
    """
    logger.info("Orchestrator: Starting event loop.")

    state = _LoopState()
    handlers = _build_dispatch_table()

    # Notify MCU that RPi is ready!
    services.tx.send_command(TxCommand.BOOT_READY, enqueue=True)

    while state.running:
        _tick_watchdog(state)
        services.tx.flush_queued_commands(limit=1)
        _monitor_flash_write(services, state)

        frame = services.rx.get_next_command()
        if frame is None:
            time.sleep(IDLE_SLEEP)
            continue

        _dispatch_command(services, frame, state, handlers)

    logger.info("Orchestrator: Event loop terminated.")


def _tick_watchdog(state: _LoopState) -> None:
    now = time.time()
    if now - state.last_watchdog_ping > config.app.WATCHDOG_INTERVAL:
        system.watchdog_ping()
        state.last_watchdog_ping = now


def _monitor_flash_write(services: Services, state: _LoopState) -> None:
    if not state.flash_write_in_progress or not services.flash.is_write_complete:
        return

    if services.flash.has_write_succeeded:
        logger.info("Orchestrator: Flash write complete. Sending WRITE_END.")
        services.tx.send_command(
            TxCommand.WRITE_END, state.pending_write_end_payload, enqueue=True
        )
    elif services.flash.has_write_failed:
        logger.error(
            "Orchestrator: Flash write failed: %s",
            services.flash.write_error or "unknown error",
        )
    else:
        logger.error("Orchestrator: Flash write ended with unknown state.")

    state.flash_write_in_progress = False
    state.pending_write_end_payload = b""


def _handle_cam_on(services: Services, frame: bytes, _state: _LoopState) -> None:
    """Handles the CAM_ON command to start an image capture mission."""
    # Extract limit from payload if present (default to 1)
    payload = services.rx.get_payload(frame)
    limit = 1
    if payload and len(payload) > 0:
        try:
            limit = int(payload[0])
        except (ValueError, TypeError):
            logger.error("Orchestrator: Invalid limit payload in CAM_ON.")

    task_handler.capture_mission(
        services.tx,
        services.storage,
        services.camera,
        services.neural_network,
        limit=limit,
    )


def _handle_image_request(services: Services, frame: bytes, _state: _LoopState) -> None:
    """Handles requests for full images or thumbnails."""
    command = services.rx.get_command(frame)
    payload = services.rx.get_payload(frame)
    is_thumbnail = command == RxCommand.THUMBNAIL_CODE
    task_handler.handle_image_request(
        services.tx, services.storage, payload, is_thumbnail
    )


def _handle_write_from(services: Services, frame: bytes, state: _LoopState) -> None:
    """Handles the WRITE_FROM command to begin writing a selected image to flash."""
    payload = services.rx.get_payload(frame)
    if payload is None:
        logger.error("Orchestrator: WRITE_FROM command is missing its payload.")
        return

    write_end_payload = task_handler.handle_image_on_flash(
        services.tx,
        services.flash,
        services.storage,
        payload,
    )
    if write_end_payload is None:
        return

    state.flash_write_in_progress = True
    state.pending_write_end_payload = write_end_payload


def _handle_abort_write(services: Services, _frame: bytes, _state: _LoopState) -> None:
    """Aborts a pending flash write operation."""
    task_handler.handle_abort_write(services.tx)


def _handle_poweroff(services: Services, _frame: bytes, state: _LoopState) -> None:
    """Signals the loop to stop and initiates system power-off."""
    state.running = task_handler.handle_power_off(services.tx)


def _handle_reboot(services: Services, _frame: bytes, state: _LoopState) -> None:
    """Signals the loop to stop and initiates a system reboot."""
    state.running = task_handler.handle_reboot(services.tx)


def _handle_report_request(
    services: Services, _frame: bytes, _state: _LoopState
) -> None:
    """Sends a database report telemetry frame."""
    task_handler.send_db_report(services.tx, services.storage)
