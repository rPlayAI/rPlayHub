@echo off
setlocal enabledelayedexpansion

cd /d "%~dp0\.."

echo === Building rPlayHub Windows GUI with MSVC ===

call "%USERPROFILE%\tools\portable-msvc\msvc\setup_x64.bat"

set CFLAGS=/nologo /c /O2 /Oi /Ot /MD /std:c11 /W3 /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS /Iclient-c /Ideps\ffmpeg-dist\include /Ideps\sdl2\include
set CXXFLAGS=/nologo /c /O2 /Oi /Ot /MD /std:c++17 /EHsc /W3 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /Iclient-c /Iclient-c\ui /Ideps\ffmpeg-dist\include /Ideps\sdl2\include /Ideps\imgui /Ideps\imgui\backends /Ideps\json

echo Compiling C sources...
cl %CFLAGS% client-c\stream-core.c /Fo:client-c\stream-core.obj
if errorlevel 1 exit /b 1
cl %CFLAGS% client-c\audio.c /Fo:client-c\audio.obj
if errorlevel 1 exit /b 1

echo Compiling UI sources...
cl %CXXFLAGS% client-c\ui\window_effects.cc /Fo:client-c\ui\window_effects.obj
if errorlevel 1 exit /b 1
cl %CXXFLAGS% client-c\ui\display_window.cc /Fo:client-c\ui\display_window.obj
if errorlevel 1 exit /b 1
cl %CXXFLAGS% client-c\ui\twin_view.cc /Fo:client-c\ui\twin_view.obj
if errorlevel 1 exit /b 1
cl %CXXFLAGS% client-c\ui\fold_view.cc /Fo:client-c\ui\fold_view.obj
if errorlevel 1 exit /b 1
cl %CXXFLAGS% client-c\ui\png_decode.cc /Fo:client-c\ui\png_decode.obj
if errorlevel 1 exit /b 1

echo Compiling ImGui sources...
cl %CXXFLAGS% deps\imgui\imgui.cpp /Fo:client-c\imgui.obj
if errorlevel 1 exit /b 1
cl %CXXFLAGS% deps\imgui\imgui_draw.cpp /Fo:client-c\imgui_draw.obj
if errorlevel 1 exit /b 1
cl %CXXFLAGS% deps\imgui\imgui_tables.cpp /Fo:client-c\imgui_tables.obj
if errorlevel 1 exit /b 1
cl %CXXFLAGS% deps\imgui\imgui_widgets.cpp /Fo:client-c\imgui_widgets.obj
if errorlevel 1 exit /b 1
cl %CXXFLAGS% deps\imgui\backends\imgui_impl_sdl2.cpp /Fo:client-c\imgui_impl_sdl2.obj
if errorlevel 1 exit /b 1
cl %CXXFLAGS% deps\imgui\backends\imgui_impl_sdlrenderer2.cpp /Fo:client-c\imgui_impl_sdlrenderer2.obj
if errorlevel 1 exit /b 1

echo Compiling rplay-gui.cpp...
cl %CXXFLAGS% client-c\rplay-gui.cpp /Fo:client-c\rplay-gui.obj
if errorlevel 1 exit /b 1

echo Compiling resources...
cd client-c
rc /nologo /fo rplay-gui.res rplay-gui.rc
cd ..
if errorlevel 1 exit /b 1

echo Linking rplay-gui.exe...
link /nologo /SUBSYSTEM:CONSOLE ^
    client-c\rplay-gui.obj ^
    client-c\stream-core.obj ^
    client-c\audio.obj ^
    client-c\ui\window_effects.obj ^
    client-c\ui\display_window.obj ^
    client-c\ui\twin_view.obj ^
    client-c\ui\fold_view.obj ^
    client-c\ui\png_decode.obj ^
    client-c\imgui.obj ^
    client-c\imgui_draw.obj ^
    client-c\imgui_tables.obj ^
    client-c\imgui_widgets.obj ^
    client-c\imgui_impl_sdl2.obj ^
    client-c\imgui_impl_sdlrenderer2.obj ^
    client-c\rplay-gui.res ^
    deps\sdl2\lib\x64\SDL2.lib ^
    deps\sdl2\lib\x64\SDL2main.lib ^
    deps\ffmpeg-dist\lib\avcodec.lib ^
    deps\ffmpeg-dist\lib\avutil.lib ^
    deps\ffmpeg-dist\lib\swscale.lib ^
    deps\ffmpeg-dist\lib\swresample.lib ^
    ws2_32.lib shell32.lib comdlg32.lib user32.lib gdi32.lib ole32.lib advapi32.lib bcrypt.lib secur32.lib winmm.lib ^
    /OUT:client-c\rplay-gui.exe
if errorlevel 1 exit /b 1

echo Copying runtime DLLs and root executable...
copy /Y "deps\sdl2\lib\x64\SDL2.dll" "client-c\" >nul
copy /Y "client-c\rplay-gui.exe" ".\" >nul
copy /Y "client-c\rplay-gui.ico" ".\" >nul

echo Build complete: client-c\rplay-gui.exe and .\rplay-gui.exe

