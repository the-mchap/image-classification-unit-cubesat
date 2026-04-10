"""
System module for ICU.
Handles low-level system operations like watchdog pings and power management.
"""

import socket
import os
import subprocess
import time
import sys
from .. import config

logger = config.logging.get_logger(__name__)

POWER_INTERVAL = 2


def notify_systemd(message: str):
    """Sends a notification message to systemd via the NOTIFY_SOCKET."""
    sock_path = os.environ.get("NOTIFY_SOCKET")
    if not sock_path:
        return
    try:
        if sock_path.startswith("@"):
            sock_path = "\0" + sock_path[1:]
        with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as sock:
            sock.connect(sock_path)
            sock.sendall(message.encode())
    except Exception as e:
        logger.debug(f"Failed to notify systemd: {e}")


def watchdog_ping():
    """Pings the systemd watchdog to indicate the application is alive."""
    notify_systemd("WATCHDOG=1")


def shutdown(action: str):
    """
    Executes a system power action (poweroff or reboot).

    Args:
        action: String "poweroff" or "reboot".
    """
    if action not in ["poweroff", "reboot"]:
        logger.error(f"System: Invalid power action '{action}'.")
        return

    logger.info(f"System: Initiating {action} sequence.")
    logger.warning(f"System: {action.capitalize()} in {POWER_INTERVAL} seconds...")
    time.sleep(POWER_INTERVAL)
    try:
        subprocess.run(["sudo", action], check=True)
    except Exception as e:
        logger.error(f"Failed to {action}: {e}")
        sys.exit(1)
