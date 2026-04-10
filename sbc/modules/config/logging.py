"""Logging configuration. Rotating file and stdout handlers."""

import logging
import sys
from logging.handlers import RotatingFileHandler
from . import loader

# Define logging constants
LOG_LEVEL = logging.INFO  # INFO is better for production
LOG_FORMAT = "%(asctime)s - %(name)s - %(levelname)s - %(message)s"
LOG_FILE = "sbc_icu.log"
# Limit log files to 5MB each, keeping up to 3 old files.
MAX_LOG_SIZE = loader.get_int("logging", "max_log_size", fallback=5) * 1024 * 1024
BACKUP_COUNT = loader.get_int("logging", "backup_count", fallback=3)

# Create a logger
logger = logging.getLogger()
logger.setLevel(LOG_LEVEL)

# Create a formatter
formatter = logging.Formatter(LOG_FORMAT)

# Create a handler for stdout
stream_handler = logging.StreamHandler(sys.stdout)
stream_handler.setFormatter(formatter)
logger.addHandler(stream_handler)

# Create a rotating handler for the log file
file_handler = RotatingFileHandler(
    LOG_FILE, 
    maxBytes=MAX_LOG_SIZE, 
    backupCount=BACKUP_COUNT
)
file_handler.setFormatter(formatter)
logger.addHandler(file_handler)


def get_logger(name):
    """Gets a configured logger."""
    return logging.getLogger(name)
