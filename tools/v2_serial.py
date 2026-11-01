"""The MPM v2's USB-C serial port (its CH343) for the tools next to this
file: pyserial from lib/pyserial (vendored, Python alone runs the tools),
else the installed one (uv run: the tools' script dependencies).

  from v2_serial import serial, find_port
"""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "lib", "pyserial"))
import serial  # noqa: E402,F401
from serial.tools import list_ports  # noqa: E402

CH343 = (0x1A86, 0x55D3)


def find_port() -> str:
    """$V2_PORT, else the first CH343."""
    env = os.environ.get("V2_PORT")
    if env:
        return env
    ports = sorted(p.device for p in list_ports.comports() if (p.vid, p.pid) == CH343)
    if not ports:
        sys.exit("No MPM v2 (CH343) found: connect its USB-C, or give the port (--port, $V2_PORT; MULTI_FLASH_PORT in CMake)")
    return ports[0]
