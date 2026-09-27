@echo off
rem Builds the desktop app, dist\RGHPort: "WiiConverted Setup.exe" (the launcher window in setup mode, with its Qt runtime
rem from platform\build\launcher), next to rghport-cli.exe, the converter it runs (PyInstaller, with numpy and
rem capstone).  Needs: pip install pyinstaller numpy capstone, and platform\build.bat (wiimote.dll) and
rem build_launcher.bat (the launcher) run before.
setlocal
cd /d "%~dp0"
python -m PyInstaller --noconfirm --clean --distpath dist --workpath build\pyinstaller packaging\RGHPort.spec
if errorlevel 1 exit /b %errorlevel%
rem the setup: the launcher window and its runtime next to the converter, the window renamed "WiiConverted Setup.exe"
xcopy /e /i /y /q "platform\build\launcher\launcher" "dist\RGHPort" >nul
if errorlevel 1 exit /b %errorlevel%
if exist "dist\RGHPort\RGHPort.exe" del /q "dist\RGHPort\RGHPort.exe"
if exist "dist\RGHPort\WiiConverted Setup.exe" del /q "dist\RGHPort\WiiConverted Setup.exe"
ren "dist\RGHPort\RGHLauncher.exe" "WiiConverted Setup.exe"
if errorlevel 1 exit /b %errorlevel%
rem the script records the converter patches into every game folder: the game does not start without them
xcopy /e /i /y /q "script_overrides" "dist\RGHPort\script_overrides" >nul
if errorlevel 1 exit /b %errorlevel%
echo.
echo Built "%~dp0dist\RGHPort\WiiConverted Setup.exe"
