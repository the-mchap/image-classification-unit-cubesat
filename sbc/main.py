"""
Main entry point for ICU software.
Initializes all components and starts the orchestrator.
"""

import subprocess
from typing import Optional
from contextlib import ExitStack

from modules import (
    config,
    get_logger,
    UARTManager,
    UARTError,
    FlashMemory,
    FlashMemoryError,
    Camera,
    NeuralNetwork,
    RxProtocol,
    TxProtocol,
    DataBase,
    orchestrator,
)

logger = get_logger(__name__)


class ApplicationError(Exception):
    """Custom exception for application-level errors."""

    pass


def bootstrap_system(
    stack: ExitStack,
) -> orchestrator.Services:
    """Initialize all classificator components and return the service container."""
    logger.info("=" * 50)
    logger.info("Initializing Classification System...")
    logger.info("=" * 50)

    try:
        # Initialize UART
        uart_manager = stack.enter_context(
            UARTManager(config.hardware.UART_PORT, config.hardware.UART_SPEED)
        )
        uart_manager.start()

        # Initialize subsystems communication protocol.
        tx_protocol = TxProtocol(uart_manager)
        rx_protocol = RxProtocol(uart_manager, tx_protocol)

        logger.info("UART protocol managers (RX/TX) initialized.")

        # Initialize DataBase (SQLite + MicroSD)
        storage = DataBase()
        logger.info("Database initialized.")

        # Initialize Flash Memory
        flash = stack.enter_context(
            FlashMemory(bus=config.hardware.SPI_BUS, device=config.hardware.SPI_DEVICE)
        )
        if not flash.is_open:
            raise ApplicationError(
                "Flash memory is required and could not be initialized."
            )
        logger.info("Flash memory initialized.")

        # Initialize Camera
        camera = stack.enter_context(
            Camera(
                width=config.app.IMAGE_WIDTH,
                height=config.app.IMAGE_HEIGHT,
                lores_width=config.app.TFLITE_WIDTH,
                lores_height=config.app.TFLITE_HEIGHT,
            )
        )
        logger.info("Camera initialized.")

        # Initialize Neural Network
        neural_network = NeuralNetwork(
            model_path=config.app.MODEL_PATH,
            input_width=config.app.TFLITE_WIDTH,
            input_height=config.app.TFLITE_HEIGHT,
        )
        logger.info("Neural network initialized.")

        return orchestrator.Services(
            rx=rx_protocol,
            tx=tx_protocol,
            flash=flash,
            storage=storage,
            camera=camera,
            neural_network=neural_network,
            uart=uart_manager,
        )

    except (UARTError, FlashMemoryError, FileNotFoundError) as e:
        raise ApplicationError(f"Failed to initialize a required component: {e}")
    except Exception as e:
        raise ApplicationError(f"An unexpected error occurred during setup: {e}")


def _start_hardware_sampler() -> Optional[subprocess.Popen]:
    """Launches the hardware sampler script if enabled in icu.conf."""
    if not config.app.HARDWARE_SAMPLING:
        return None

    output_file = open(config.app.SAMPLER_OUTPUT, "w")
    process = subprocess.Popen(
        ["bash", config.app.SAMPLER_SCRIPT, str(config.app.SAMPLING_INTERVAL)],
        stdout=output_file,
    )
    logger.info(
        "Hardware sampler started (interval: %ds, output: %s)",
        config.app.SAMPLING_INTERVAL,
        config.app.SAMPLER_OUTPUT,
    )
    return process


def main() -> None:
    """Main application entry point."""
    services: Optional[orchestrator.Services] = None
    sampler: Optional[subprocess.Popen] = None

    try:
        sampler = _start_hardware_sampler()

        with ExitStack() as stack:
            services = bootstrap_system(stack)
            logger.info("Initialization complete. Starting orchestrator.")

            orchestrator.start_event_loop(services)

    except (KeyboardInterrupt, SystemExit):
        logger.info("Shutdown signal received.")
    except ApplicationError as e:
        logger.error(f"Application error: {e}")
    except Exception as e:
        logger.error(f"Unexpected fatal error: {e}", exc_info=True)
    finally:
        if sampler:
            sampler.terminate()
            sampler.wait()
            logger.info("Hardware sampler stopped.")
        logger.info("Application Shutdown Complete")


if __name__ == "__main__":
    main()
