@echo off
rem Build the PC-8801 PC test harness (Visual Studio 2026, x64)
rem   build.bat [outdir] [srcdir]
rem     outdir : objects and pc88host.exe go here (default: host\build)
rem     srcdir : folder with pc88*.cpp, Z80.c ... (default: the project folder)
setlocal
set "OUT=%~1"
if "%OUT%"=="" set "OUT=%~dp0build"
if not exist "%OUT%" mkdir "%OUT%"
set "SRC=%~2"
if "%SRC%"=="" set "SRC=%~dp0.."
set "PATH=C:\Program Files (x86)\Microsoft Visual Studio\Installer;%PATH%"
call "C:\Program Files\Microsoft Visual Studio\18\Professional\VC\Auxiliary\Build\vcvars64.bat" >nul
set "INC=/I"%~dp0." /I"%SRC%""
rem main Z80 with DEBUG (instruction trace); z80sub.c drops DEBUG by itself
cl /nologo /c /O2 /W1 /utf-8 /DDEBUG /DPC88_SUB_TRACE /DZ80_OPSTATS /wd4068 %INC% /Fo"%OUT%/" /TC "%SRC%\Z80.c" "%SRC%\z80sub.c" || exit /b 1
cl /nologo /c /O2 /W3 /wd4068 /wd5033 /wd4996 /wd4244 /utf-8 /EHsc /std:c++17 /D_USE_MATH_DEFINES /DPC88_SUB_TRACE %INC% /Fo"%OUT%/" "%SRC%\pc88_core.cpp" "%SRC%\pc88_video.cpp" "%SRC%\pc88_disk.cpp" "%SRC%\pc88_fdc.cpp" "%SRC%\ym2203.cpp" "%~dp0main.cpp" "%~dp0test_fdc.cpp" || exit /b 1
link /nologo /OUT:"%OUT%\pc88host.exe" "%OUT%\Z80.obj" "%OUT%\z80sub.obj" "%OUT%\pc88_core.obj" "%OUT%\pc88_video.obj" "%OUT%\pc88_disk.obj" "%OUT%\pc88_fdc.obj" "%OUT%\ym2203.obj" "%OUT%\main.obj" "%OUT%\test_fdc.obj" || exit /b 1
echo built %OUT%\pc88host.exe
endlocal
