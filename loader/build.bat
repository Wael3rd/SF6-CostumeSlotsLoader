@echo off
setlocal
cd /d "%~dp0"

call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1

set CF=/nologo /std:c++17 /O2 /MT /EHa /W3 /D_CRT_SECURE_NO_WARNINGS /I"third_party"
set T3=third_party\zstddeclib.c third_party\miniz.c
set LK=/link /INCREMENTAL:NO

echo === paktool.exe ===
cl %CF% %T3% pak.cpp patch.cpp paktool.cpp /Fe"paktool.exe" %LK%
if %ERRORLEVEL% neq 0 ( echo FAILED & exit /b 1 )

echo === costume_loader.exe ===
cl %CF% %T3% pak.cpp patch.cpp loader_core.cpp costume_loader_main.cpp /Fe"costume_loader.exe" %LK% bcrypt.lib
if %ERRORLEVEL% neq 0 ( echo FAILED & exit /b 1 )

echo === amd_ags_x64.dll ===
cl %CF% /I"proxy" %T3% pak.cpp patch.cpp loader_core.cpp proxy\amd_ags_proxy.cpp /LD /Fe"amd_ags_x64.dll" %LK% bcrypt.lib /MACHINE:X64
if %ERRORLEVEL% neq 0 ( echo FAILED & exit /b 1 )

echo === loadtest.exe ===
cl /nologo /std:c++17 /O2 /MT /EHa /W3 /D_CRT_SECURE_NO_WARNINGS loadtest.cpp /Fe"loadtest.exe" %LK%
if %ERRORLEVEL% neq 0 ( echo FAILED & exit /b 1 )

echo.
echo All OK: paktool.exe costume_loader.exe amd_ags_x64.dll loadtest.exe
