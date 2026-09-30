@echo off
setlocal
cd /d "%~dp0"

rem Builds amd_ags_x64.dll = the costume loader (its own sources, unchanged) + stage slots.
rem COSTUME_SRC: folder holding the costume loader's loader sources and its archive_deps.lib.
if "%COSTUME_SRC%"=="" set "COSTUME_SRC=C:\Program Files (x86)\Steam\steamapps\common\Street Fighter 6\reframework\SF6_CostumeLoader"
if not exist "%COSTUME_SRC%\loader_core.cpp" goto :nosrc
if not exist "%COSTUME_SRC%\archive_deps.lib" goto :nodeps

set "VSDIR=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1

if not exist ..\build\obj mkdir ..\build\obj
set "CS=%COSTUME_SRC%"
set CF=/nologo /std:c++17 /O2 /MT /EHa /W3 /D_CRT_SECURE_NO_WARNINGS /I"%CS%" /I"%CS%\proxy" /I"%CS%\third_party"
set SYSLIBS=bcrypt.lib advapi32.lib shell32.lib ole32.lib user32.lib

echo === amd_ags_x64.dll (costume loader + stage slots) ===
cl %CF% /Fo..\build\obj\ "%CS%\third_party\zstddeclib.c" "%CS%\third_party\miniz.c" "%CS%\pak.cpp" "%CS%\patch.cpp" "%CS%\archive.cpp" "%CS%\loader_core.cpp" stage_log.cpp stage_loader.cpp preview_tex.cpp stage_redirect.cpp proxy\slots_proxy.cpp /LD /Fe..\build\amd_ags_x64.dll /link /INCREMENTAL:NO "%CS%\archive_deps.lib" %SYSLIBS% /MACHINE:X64
if errorlevel 1 goto :failed

echo.
echo OK: build\amd_ags_x64.dll
exit /b 0

:failed
echo FAILED
exit /b 1
:nosrc
echo COSTUME_SRC not found: "%COSTUME_SRC%"
exit /b 1
:nodeps
echo archive_deps.lib missing: run the costume loader's build.bat first
exit /b 1
