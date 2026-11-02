"""The game executable the harness launches by default: dist/uc.exe on Windows, dist/uc elsewhere."""
import sys

DEFAULT_EXE_NAME = "uc.exe" if sys.platform == "win32" else "uc"
DEFAULT_EXE = "dist/" + DEFAULT_EXE_NAME
