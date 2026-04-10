# modules/config/__init__.py
"""
Configuration package for the application.
This package consolidates all configuration settings, divided into
domain-specific modules.
"""

from . import app
from . import hardware
from . import logging
from . import protocol

__all__ = ["app", "hardware", "logging", "protocol"]
