@echo off
setlocal
cd /d "%~dp0"
if "%COSTUME_SRC%"=="" set "COSTUME_SRC=C:\Program Files (x86)\Steam\steamapps\common\Street Fighter 6\reframework\SF6_CostumeLoader"
set "VSDIR=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if not exist ..\build\tools_obj mkdir ..\build\tools_obj
set "CS=%COSTUME_SRC%"
cl /nologo /std:c++17 /O2 /MT /EHa /W3 /D_CRT_SECURE_NO_WARNINGS /I"%CS%" /I"%CS%\third_party" /Fo..\build\tools_obj\ "%CS%\third_party\zstddeclib.c" "%CS%\third_party\miniz.c" "%CS%\pak.cpp" stagepak.cpp /Fe..\build\stagepak.exe /link /INCREMENTAL:NO
if errorlevel 1 exit /b 1
echo OK: build\stagepak.exe

cl /nologo /std:c++17 /O2 /MT /EHa /W3 /D_CRT_SECURE_NO_WARNINGS /I..\loader /Fo..\build\tools_obj\ ..\loader\preview_tex.cpp textest.cpp /Fe..\build\textest.exe /link /INCREMENTAL:NO
if errorlevel 1 exit /b 1
echo OK: build\textest.exe
cl /nologo /std:c++17 /O2 /MT /EHa /W3 /D_CRT_SECURE_NO_WARNINGS /I"%CS%" /I"%CS%\third_party" /Fo..\build\tools_obj\ "%CS%\third_party\zstddeclib.c" "%CS%\third_party\miniz.c" "%CS%\pak.cpp" "%CS%\archive.cpp" ..\loader\stage_log.cpp ..\loader\stage_loader.cpp ..\loader\preview_tex.cpp stagetest.cpp /Fe..\build\stagetest.exe /link /INCREMENTAL:NO "%CS%\archive_deps.lib" bcrypt.lib advapi32.lib shell32.lib ole32.lib user32.lib
if errorlevel 1 exit /b 1
echo OK: build\stagetest.exe
