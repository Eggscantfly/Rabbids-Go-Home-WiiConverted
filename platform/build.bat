@echo off
REM Build the platform DLL (wiimote.dll, 32-bit like the PC executable) and its offline test wmtest.exe
REM into build\ next to this script.  Visual Studio with the C++ x86 tools is located with vswhere.

setlocal EnableExtensions
set "HERE=%~dp0"
set "OUT=%HERE%build"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto no_vswhere

set "VSDIR="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR goto no_vs
if not exist "%VSDIR%\VC\Auxiliary\Build\vcvars32.bat" goto no_vs

REM A minimal PATH: some inherited PATH entries break vcvars32.bat.
set "PATH=%SystemRoot%\system32;%SystemRoot%;%SystemRoot%\System32\Wbem;%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
call "%VSDIR%\VC\Auxiliary\Build\vcvars32.bat" >nul
if errorlevel 1 goto vcvars_failed

if not exist "%OUT%\obj" mkdir "%OUT%\obj"
pushd "%HERE%"

cl /nologo /LD /O2 /Zi /MT /EHsc /W3 /DNDEBUG /D_CRT_SECURE_NO_WARNINGS /I lua ^
   /Fo"%OUT%\obj\\" /Fd"%OUT%\obj\\" ^
   wm_main.cpp wm_input.cpp wm_proto.cpp wm_patch.cpp wm_sav.cpp wm_ctl.cpp wm_video.cpp wm_afx.cpp wm_timing.cpp wm_audio.cpp wm_script.cpp wm_options.cpp wm_gfx.cpp wm_hang.cpp wm_fixes.cpp wm_motion.cpp wm_mods.cpp wm_lua.cpp wm_natives_table.cpp wm_sm64.cpp wm_media.cpp lua\lua_all.c vendor\stb_vorbis.c ^
   /link /OUT:"%OUT%\wiimote.dll" /DEF:wiimote.def /IMPLIB:"%OUT%\obj\wiimote.lib" /MACHINE:X86 /SUBSYSTEM:WINDOWS /DEBUG /PDB:"%OUT%\wiimote.pdb" /OPT:REF /OPT:ICF ^
   ws2_32.lib winmm.lib user32.lib
if errorlevel 1 goto dll_failed

cl /nologo /O2 /MT /EHsc /W3 /D_CRT_SECURE_NO_WARNINGS /D_WINSOCK_DEPRECATED_NO_WARNINGS ^
   /Fo"%OUT%\obj\\" /Fd"%OUT%\obj\\" ^
   wmtest.cpp ^
   /link /OUT:"%OUT%\wmtest.exe" /MACHINE:X86 /SUBSYSTEM:CONSOLE ws2_32.lib
if errorlevel 1 goto test_failed

copy /y wiimote.ini "%OUT%\wiimote.ini" >nul
popd
echo.
echo built build\wiimote.dll and build\wmtest.exe (configuration template: build\wiimote.ini)
endlocal
exit /b 0

:no_vswhere
echo [!] vswhere.exe not found: install Visual Studio 2017 or later with the C++ desktop tools
exit /b 1
:no_vs
echo [!] no Visual Studio installation with the C++ x86/x64 build tools was found
exit /b 1
:vcvars_failed
echo [!] vcvars32.bat failed
exit /b 1
:dll_failed
popd
echo [!] wiimote.dll build failed
exit /b 1
:test_failed
popd
echo [!] wmtest.exe build failed
exit /b 1
