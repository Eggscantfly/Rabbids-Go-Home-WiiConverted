@echo off
REM Builds the native LZO module, rghport\archive\_lzo_native.<python tag>.pyd (Pybind11 + miniLZO), with CMake and
REM Ninja; pip install . builds the same module through setup.py.
setlocal

set "SRC=%~dp0."
set "ROOT=%~dp0.."
set "BUILD=%~dp0build"

REM Visual Studio's x64 tools: VCVARS when set (a vcvars64.bat), else the newest installation vswhere finds.
REM Without either, the build runs in the developer prompt it was started from.
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not defined VCVARS if exist "%VSWHERE%" for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VCVARS=%%i\VC\Auxiliary\Build\vcvars64.bat"
if defined VCVARS if exist "%VCVARS%" call "%VCVARS%" >nul

cmake -S "%SRC%" -B "%BUILD%" -G Ninja -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 exit /b %errorlevel%

cmake --build "%BUILD%" --config Release
if errorlevel 1 exit /b %errorlevel%

echo.
echo Built native LZO module in "%ROOT%\rghport\archive"
