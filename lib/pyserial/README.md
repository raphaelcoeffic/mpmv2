# pyserial

The serial ports for the flashing scripts, so that they run with Python
alone (no pip, venv or uv): the MPM v2's [tools](../../tools)
(`stk500_flash.py`, `rom_flash.py`, `mpm_fs.py`) and, in the MULTI-Module
repo, `cmake/multi_flash.py` (STM32). They add this directory to `sys.path`.

[pyserial](https://github.com/pyserial/pyserial) 3.5, from the PyPI wheel
(`pyserial-3.5-py2.py3-none-any.whl`, SHA-256
c4451db6ba391ca6ca299fb3ec7bae67a5c55dde170964c7a14ceefec02f2cf0), BSD-3-Clause
([serial/LICENSE.txt](serial/LICENSE.txt), from the source archive). The
serial ports of Windows, macOS and Linux and their listing, unchanged; left
out: the URL handlers, RFC 2217, miniterm, the Java and .NET backends.
