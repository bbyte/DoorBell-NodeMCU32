import os
import sys
import pytest

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), '..')))
from session_logger import strip_ansi


def test_strip_ansi_removes_escape_sequences():
    colored = "\x1b[31mError:\x1b[0m Something happened"
    assert strip_ansi(colored) == "Error: Something happened"
