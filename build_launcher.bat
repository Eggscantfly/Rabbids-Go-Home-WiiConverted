@echo off
REM Builds the game folder's launcher (launcher\: RGHLauncher.exe, a Qt Widgets window, and the stub
REM "WiiConverted Launcher.exe") and deploys it with its Qt runtime into platform\build\launcher\, which rghport assemble
REM copies into every game folder.  Needs Visual Studio (C++ x64 tools), CMake, Ninja and a Qt 6 MSVC 64-bit kit:
REM QT_DIR, else the newest C:\Qt\<version>\msvc*_64.  The stub's icon: LAUNCHER_ICON, else Assets\GUI\WC icon.ico.

setlocal EnableExtensions
set "HERE=%~dp0"
set "OUT=%HERE%platform\build\launcher"
set "BUILD=%HERE%build\launcher"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto no_vswhere

set "VSDIR="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR goto no_vs

if not defined QT_DIR (
  for /d %%q in ("C:\Qt\6.*") do for /d %%k in ("%%q\msvc*_64") do set "QT_DIR=%%k"
)
if not defined QT_DIR goto no_qt
if not exist "%QT_DIR%\bin\windeployqt.exe" goto no_qt

REM Ninja from pip lives in the Scripts folder of the Python on PATH (looked up before PATH is trimmed)
set "PYSCRIPTS="
for /f "usebackq delims=" %%p in (`python -c "import os,sys;print(os.path.join(sys.prefix,'Scripts'))" 2^>nul`) do set "PYSCRIPTS=%%p"
set "PATH=%SystemRoot%\system32;%SystemRoot%;%SystemRoot%\System32\Wbem;%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%ProgramFiles%\CMake\bin;%PYSCRIPTS%;%QT_DIR%\bin"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 goto vcvars_failed

if not defined LAUNCHER_ICON set "LAUNCHER_ICON=%HERE%Assets\GUI\WC icon.ico"
copy /y "%LAUNCHER_ICON%" "%HERE%launcher\stub\stub.ico" >nul
if errorlevel 1 goto icon_failed

cmake -S "%HERE%launcher" -B "%BUILD%" -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_PREFIX_PATH=%QT_DIR%"
if errorlevel 1 goto build_failed
cmake --build "%BUILD%"
if errorlevel 1 goto build_failed

if exist "%OUT%" rmdir /s /q "%OUT%"
mkdir "%OUT%\launcher"
copy /y "%BUILD%\RGHLauncher.exe" "%OUT%\launcher\RGHLauncher.exe" >nul
copy /y "%BUILD%\WiiConverted Launcher.exe" "%OUT%\WiiConverted Launcher.exe" >nul
windeployqt --release --no-translations --no-system-d3d-compiler --no-system-dxc-compiler --no-opengl-sw ^
  --no-compiler-runtime --skip-plugin-types imageformats,iconengines,generic,networkinformation,tls,styles ^
  "%OUT%\launcher\RGHLauncher.exe"
if errorlevel 1 goto deploy_failed
REM the C++ runtime the Qt libraries and the launcher need, from the Visual Studio redistributables
for /d %%r in ("%VCToolsRedistDir%x64\Microsoft.VC*.CRT") do copy /y "%%r\*.dll" "%OUT%\launcher\" >nul
if not exist "%OUT%\launcher\vcruntime140.dll" goto runtime_failed
del /q "%HERE%launcher\stub\stub.ico"
echo.
echo built platform\build\launcher\ ("WiiConverted Launcher.exe" + launcher\RGHLauncher.exe with its Qt runtime)
endlocal
exit /b 0

:no_vswhere
echo [!] vswhere.exe not found: install Visual Studio 2017 or later with the C++ desktop tools
exit /b 1
:no_vs
echo [!] no Visual Studio installation with the C++ x86/x64 build tools was found
exit /b 1
:no_qt
echo [!] no Qt 6 MSVC 64-bit kit found: set QT_DIR to one (for example C:\Qt\6.8.3\msvc2022_64)
exit /b 1
:vcvars_failed
echo [!] vcvars64.bat failed
exit /b 1
:icon_failed
echo [!] the stub's icon "%LAUNCHER_ICON%" could not be copied
exit /b 1
:build_failed
echo [!] the launcher build failed
exit /b 1
:deploy_failed
echo [!] windeployqt failed
exit /b 1
:runtime_failed
echo [!] the C++ runtime DLLs were not found under VCToolsRedistDir
exit /b 1
