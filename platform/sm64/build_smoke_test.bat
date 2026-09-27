@echo off
REM Build smoke_test.exe (32-bit, like sm64.dll) into build\ with the same clang / mingw32 sysroot as build.bat.
setlocal EnableExtensions
set "HERE=%~dp0"
set "HERE=%HERE:~0,-1%"
if not defined CLANG for /f "delims=" %%i in ('where clang 2^>nul') do if not defined CLANG set "CLANG=%%i"
if not defined CLANG set "CLANG=%ProgramFiles%\LLVM\bin\clang.exe"
if not defined SYSROOT set "SYSROOT=%SystemDrive%/msys64/mingw32"
"%CLANG%" --target=i686-w64-mingw32 --sysroot=%SYSROOT% -O1 -w -fuse-ld=lld -static -I "%HERE%" -o "%HERE%\build\smoke_test.exe" "%HERE%\smoke_test.c"
if errorlevel 1 exit /b 1
echo built build\smoke_test.exe: run it in build\ with the ROM's path
endlocal
