# -*- mode: python ; coding: utf-8 -*-
"""PyInstaller build of the converter, dist/RGHPort/rghport-cli.exe, with its _internal folder: the command line the
setup runs for every conversion and every folder check.  The setup itself, "WiiConverted Setup.exe", is the launcher window in
setup mode: build_app.bat copies it with its Qt runtime next to the converter after this build.

Bundled besides the code: the platform DLL built from platform/ and its configuration template, and the deployed
launcher (rghport assemble copies them into a port folder).  No game data."""
import os

from PyInstaller.utils.hooks import collect_submodules

ROOT = os.path.abspath(os.path.join(SPECPATH, ".."))
DLL = os.path.join(ROOT, "platform", "build", "wiimote.dll")
if not os.path.isfile(DLL):
    raise SystemExit("platform/build/wiimote.dll is missing: run platform/build.bat first")
LAUNCHER = os.path.join(ROOT, "platform", "build", "launcher")
if not os.path.isfile(os.path.join(LAUNCHER, "WiiConverted Launcher.exe")):
    raise SystemExit("platform/build/launcher is missing: run build_launcher.bat first")

hidden = collect_submodules("rghport")
datas = [(DLL, os.path.join("platform", "build")),
         (LAUNCHER, os.path.join("platform", "build", "launcher")),
         (os.path.join(ROOT, "platform", "wiimote.ini"), "platform"),
         (os.path.join(ROOT, "rghport", "data", "moon.ico"), os.path.join("rghport", "data"))]
excludes = ["tkinter", "PIL", "matplotlib", "scipy", "IPython", "pytest", "setuptools", "pip", "PySide6", "shiboken6"]

cli = Analysis([os.path.join(ROOT, "packaging", "cli_entry.py")], pathex=[ROOT], hiddenimports=hidden, datas=datas,
               excludes=excludes, noarchive=False)
cli_exe = EXE(PYZ(cli.pure), cli.scripts, [], exclude_binaries=True, name="rghport-cli", console=True, icon="NONE",
              upx=False)
COLLECT(cli_exe, cli.binaries, cli.datas, upx=False, name="RGHPort")
