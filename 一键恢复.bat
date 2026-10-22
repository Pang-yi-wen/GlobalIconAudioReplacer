@echo off
chcp 936 >nul
cd /d "%~dp0"

echo.
echo   Global Icon Replacer - restore
echo   ----------------------------------------
echo   This will restore ALL of the following:
echo     - file type icons in the registry
echo     - shortcut (.lnk) icons
echo     - Explorer thumbnail settings
echo   Explorer will be restarted to apply changes.
echo   ----------------------------------------
echo.
pause

IconReplacer.exe restore

echo.
echo   === restore finished ===
pause
