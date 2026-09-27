# The game folder's launcher and the setup: RGHLauncher.exe (Qt Widgets, the window: the launcher in a game folder,
# the setup anywhere else) and "WiiConverted Launcher.exe" (the stub that starts it in the game folder and carries the
# icon).  Built by build_launcher.bat in the repository root; the deployed tree goes to platform/build/launcher/, which
# rghport assemble copies into every game folder and build_app.bat copies next to rghport-cli.exe as the setup.
cmake_minimum_required(VERSION 3.20)
project(RGHLauncher LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets)

qt_add_executable(RGHLauncher WIN32 main.cpp ui.cpp ui.h setup.cpp setup.h settings.cpp settings.h music.cpp music.h sfx.cpp sfx.h
                  meter.cpp meter.h
                  ../platform/vendor/stb_vorbis.c launcher.qrc launcher.rc)
target_link_libraries(RGHLauncher PRIVATE Qt6::Widgets dwmapi winmm)
target_compile_definitions(RGHLauncher PRIVATE UNICODE _UNICODE)

# the stub: plain Win32, /FIXED so .rsrc stays the last section (rghport --icon rewrites it)
add_executable(stub WIN32 stub/stub.cpp stub/stub.rc stub/stub.manifest)
set_target_properties(stub PROPERTIES AUTOMOC OFF AUTORCC OFF)
set_target_properties(stub PROPERTIES OUTPUT_NAME "WiiConverted Launcher")
target_link_options(stub PRIVATE /FIXED /DYNAMICBASE:NO)
target_compile_definitions(stub PRIVATE UNICODE _UNICODE)
