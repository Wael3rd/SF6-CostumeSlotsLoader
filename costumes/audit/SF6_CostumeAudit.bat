@echo off
rem Lists the costume mods installed and the outfits the loader made of them. Changes nothing.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0SF6_CostumeAudit.ps1"
echo.
pause
