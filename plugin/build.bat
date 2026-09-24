@echo off
rem Build SF6_CostumeSlotsNative.dll — headers API v1.5.8, /EHa (regle 17)
rem Appeler par chemin absolu. Sortie dans out\ (ne pas copier dans plugins\ pendant que le jeu tourne).
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0"
if not exist out mkdir out
cl /nologo /std:c++20 /EHa /O2 /W4 /LD /I include SF6_CostumeSlotsNative.cpp /Fe:out\SF6_CostumeSlotsNative.dll /Fo:out\SF6_CostumeSlotsNative.obj /link /opt:ref
if errorlevel 1 (
    echo ECHEC de la compilation.
    exit /b 1
)
echo.
echo   OK : out\SF6_CostumeSlotsNative.dll
echo   NE PAS copier dans plugins\ pendant que le jeu tourne.
