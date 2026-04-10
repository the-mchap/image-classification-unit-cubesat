"""Loads user-tunable parameters from icu.conf in the project root."""

import configparser
import os

_PROJECT_ROOT = os.path.dirname(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
)
_CONF_PATH = os.path.join(_PROJECT_ROOT, "icu.conf")

_parser = configparser.ConfigParser()
_parser.read(_CONF_PATH)


def get_int(section: str, key: str, fallback: int) -> int:
    return _parser.getint(section, key, fallback=fallback)


def get_float(section: str, key: str, fallback: float) -> float:
    return _parser.getfloat(section, key, fallback=fallback)


def get_bool(section: str, key: str, fallback: bool) -> bool:
    return _parser.getboolean(section, key, fallback=fallback)
