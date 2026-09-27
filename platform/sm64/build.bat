@echo off
REM Build libsm64 as the 32-bit sm64.dll the platform DLL loads ([sm64] in wiimote.ini), into build\ next to this
REM script.  The library's sources are the SM64 decompilation's and use GCC attributes MSVC does not take, so this
REM builds them with clang targeting mingw32 (LLVM for Windows, and an MSYS2 mingw32 sysroot for the C runtime's
REM headers and import libraries).  Set CLANG and SYSROOT below if yours are elsewhere.
REM
REM The source folders are the library's own (its Makefile's SRC_DIRS) plus the object engine's (import-objects.py
REM writes src\decomp\actors, src\decomp\data and src\decomp\levels from the decompilation; the behaviours under
REM src\decomp\game\behaviors and the actors' own folders are #included, not compiled), listed rather than walked:
REM src\decomp\audio\copt holds an include fragment that is not a translation unit of its own.

setlocal EnableExtensions EnableDelayedExpansion
set "HERE=%~dp0"
set "SRC=%HERE%libsm64-master"
set "OUT=%HERE%build"

REM clang off PATH, else LLVM's own installation; the sysroot where MSYS2 puts it on the system drive
if not defined CLANG for /f "delims=" %%i in ('where clang 2^>nul') do if not defined CLANG set "CLANG=%%i"
if not defined CLANG set "CLANG=%ProgramFiles%\LLVM\bin\clang.exe"
if not defined SYSROOT set "SYSROOT=%SystemDrive%/msys64/mingw32"

if not exist "%CLANG%" goto no_clang
if not exist "%SYSROOT%\include\stdio.h" goto no_sysroot
if not exist "%SRC%\src\libsm64.c" goto no_src
if not exist "%SRC%\src\decomp\actors\model_table.c" goto no_import

if not exist "%OUT%\obj" mkdir "%OUT%\obj"

set "DIRS=src src\decomp src\decomp\engine src\decomp\include\PR src\decomp\game src\decomp\pc"
set "DIRS=%DIRS% src\decomp\pc\audio src\decomp\mario src\decomp\tools src\decomp\audio"
set "DIRS=%DIRS% src\decomp\data src\decomp\actors src\decomp\levels"

REM -g and frame pointers: a crash inside sm64.dll is logged by the platform as module + offset, and
REM llvm-symbolizer --obj=sm64.dll --relative-address turns that into a function and a line
set "CFLAGS=--target=i686-w64-mingw32 --sysroot=%SYSROOT% -fno-strict-aliasing -O2 -w -g -gdwarf-4 -fno-omit-frame-pointer"
set "CFLAGS=%CFLAGS% -DSM64_LIB_EXPORT -DGBI_FLOATS -DVERSION_US -DNO_SEGMENTED_MEMORY"
set "CFLAGS=%CFLAGS% -I "%SRC%\src\decomp\include" -I "%SRC%\src\decomp" -I "%SRC%\src""

echo Compiling libsm64 (32-bit, clang -^> mingw32)...
set "OBJS="
set /a N=0
for %%d in (%DIRS%) do (
    for %%f in ("%SRC%\%%d\*.c") do (
        set /a N+=1
        set "OBJ=%%d_%%~nf"
        set "OBJ=!OBJ:\=_!"
        "%CLANG%" %CFLAGS% -c "%%~ff" -o "%OUT%\obj\!OBJ!.o"
        if errorlevel 1 goto compile_failed
        set "OBJS=!OBJS! "%OUT%\obj\!OBJ!.o""
    )
)
echo   !N! source files

echo Linking sm64.dll...
"%CLANG%" --target=i686-w64-mingw32 --sysroot=%SYSROOT% -fuse-ld=lld -shared -static -o "%OUT%\sm64.dll" !OBJS! -lm
if errorlevel 1 goto link_failed

echo.
echo built build\sm64.dll - put it next to the game's executable, with your own Super Mario 64 (USA) .z64
echo named by [sm64] rom= in wiimote.ini
endlocal
exit /b 0

:compile_failed
echo.
echo A source file did not compile.  See the error above.
exit /b 1

:link_failed
echo.
echo sm64.dll did not link.  See the error above.
exit /b 1

:no_clang
echo clang was not found at "%CLANG%".
echo Install LLVM for Windows, or set CLANG to your clang.exe, and try again.
exit /b 1

:no_sysroot
echo No mingw32 sysroot at "%SYSROOT%".
echo Install MSYS2's mingw-w64-i686 toolchain, or set SYSROOT to yours, and try again.
exit /b 1

:no_src
echo The libsm64 sources are not in "%SRC%".
echo Put libsm64's tree there (its src\libsm64.c must exist) and try again.
exit /b 1

:no_import
echo The object engine has not been imported: run import-objects.py (with the sm64-master folder) first.
exit /b 1
