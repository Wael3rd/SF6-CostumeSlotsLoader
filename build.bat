@echo off
setlocal
cd /d "%~dp0"

rem Visual Studio or Build Tools with the C++ workload, wherever it is installed
set VSDIR=C:\Program Files\Microsoft Visual Studio\2022\Community
set VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe
if exist "%VSWHERE%" for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1

set CF=/nologo /std:c++17 /O2 /MT /EHa /W3 /D_CRT_SECURE_NO_WARNINGS /I"third_party"
set T3=third_party\zstddeclib.c third_party\miniz.c
set LK=/link /INCREMENTAL:NO
rem Libraries the archive decoders may import. All of them are already loaded by the game's own
rem executable, so importing them statically adds nothing at runtime.
set SYSLIBS=bcrypt.lib advapi32.lib shell32.lib ole32.lib user32.lib

rem ---------------------------------------------------------------------------------------------
rem Archive decoders (7z: LZMA SDK, rar: UnRAR), built once into archive_deps.lib.
rem Delete archive_deps.lib, or run "build.bat deps", to rebuild them.
rem UnRAR is built single-threaded and without Crypt32: see third_party\unrar\SF6_PATCHES.txt.
rem ---------------------------------------------------------------------------------------------
if /i "%1"=="deps" if exist archive_deps.lib del archive_deps.lib
if exist archive_deps.lib goto :targets

echo === archive_deps.lib (LZMA SDK + UnRAR) ===
if exist obj\deps rmdir /s /q obj\deps
mkdir obj\deps
set LZ=third_party\lzma
cl /nologo /O2 /MT /W1 /c /Foobj\deps\ %LZ%\7zAlloc.c %LZ%\7zArcIn.c %LZ%\7zBuf.c %LZ%\7zBuf2.c %LZ%\7zCrc.c %LZ%\7zCrcOpt.c %LZ%\7zDec.c %LZ%\7zFile.c %LZ%\7zStream.c %LZ%\Bcj2.c %LZ%\Bra.c %LZ%\Bra86.c %LZ%\BraIA64.c %LZ%\CpuArch.c %LZ%\Delta.c %LZ%\Lzma2Dec.c %LZ%\LzmaDec.c %LZ%\Ppmd7.c %LZ%\Ppmd7Dec.c
if %ERRORLEVEL% neq 0 ( echo FAILED lzma & exit /b 1 )
set UR=third_party\unrar
set URDEF=/DRARDLL /DUNRAR /DSILENT /DSF6_LOADER_NO_SMP /DSF6_LOADER_NO_CRYPT32
cl /nologo /std:c++17 /O2 /MT /EHa /W1 %URDEF% /c /Foobj\deps\ %UR%\archive.cpp %UR%\arcread.cpp %UR%\blake2s.cpp %UR%\cmddata.cpp %UR%\consio.cpp %UR%\crc.cpp %UR%\crypt.cpp %UR%\dll.cpp %UR%\encname.cpp %UR%\errhnd.cpp %UR%\extinfo.cpp %UR%\extract.cpp %UR%\filcreat.cpp %UR%\file.cpp %UR%\filefn.cpp %UR%\filestr.cpp %UR%\find.cpp %UR%\getbits.cpp %UR%\global.cpp %UR%\hash.cpp %UR%\headers.cpp %UR%\isnt.cpp %UR%\largepage.cpp %UR%\match.cpp %UR%\motw.cpp %UR%\options.cpp %UR%\pathfn.cpp %UR%\qopen.cpp %UR%\rar.cpp %UR%\rarvm.cpp %UR%\rawread.cpp %UR%\rdwrfn.cpp %UR%\rijndael.cpp %UR%\rs.cpp %UR%\rs16.cpp %UR%\scantree.cpp %UR%\secpassword.cpp %UR%\sha1.cpp %UR%\sha256.cpp %UR%\smallfn.cpp %UR%\strfn.cpp %UR%\strlist.cpp %UR%\system.cpp %UR%\threadpool.cpp %UR%\timefn.cpp %UR%\ui.cpp %UR%\unicode.cpp %UR%\unpack.cpp %UR%\volume.cpp
if %ERRORLEVEL% neq 0 ( echo FAILED unrar & exit /b 1 )
lib /nologo /OUT:archive_deps.lib obj\deps\*.obj
if %ERRORLEVEL% neq 0 ( echo FAILED lib & exit /b 1 )

:targets
echo === paktool.exe ===
cl %CF% %T3% pak.cpp patch.cpp paktool.cpp /Fe"paktool.exe" %LK%
if %ERRORLEVEL% neq 0 ( echo FAILED & exit /b 1 )

echo === costume_loader.exe ===
cl %CF% %T3% pak.cpp patch.cpp archive.cpp loader_core.cpp costume_loader_main.cpp /Fe"costume_loader.exe" %LK% archive_deps.lib %SYSLIBS%
if %ERRORLEVEL% neq 0 ( echo FAILED & exit /b 1 )

echo === amd_ags_x64.dll ===
cl %CF% /I"proxy" %T3% pak.cpp patch.cpp archive.cpp loader_core.cpp proxy\amd_ags_proxy.cpp /LD /Fe"amd_ags_x64.dll" %LK% archive_deps.lib %SYSLIBS% /MACHINE:X64
if %ERRORLEVEL% neq 0 ( echo FAILED & exit /b 1 )

echo === loadtest.exe ===
cl /nologo /std:c++17 /O2 /MT /EHa /W3 /D_CRT_SECURE_NO_WARNINGS loadtest.cpp /Fe"loadtest.exe" %LK%
if %ERRORLEVEL% neq 0 ( echo FAILED & exit /b 1 )

echo.
echo All OK: paktool.exe costume_loader.exe amd_ags_x64.dll loadtest.exe
