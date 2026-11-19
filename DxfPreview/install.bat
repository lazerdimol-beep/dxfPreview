@echo off
rem Installs the DXF thumbnail + preview extension for all users (needs administrator rights).
setlocal
net session >nul 2>&1
if %errorlevel% neq 0 (
  echo Requesting administrator rights...
  powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
  exit /b
)
cd /d "%~dp0"
set "SRC=%~dp0DxfPreview.dll"
if not exist "%SRC%" set "SRC=%~dp0build\DxfPreview.dll"
if not exist "%SRC%" (
  echo [!] DxfPreview.dll not found. Build it first with build.bat.
  pause
  exit /b 1
)
set "DEST=%ProgramFiles%\DxfPreview"
if not exist "%DEST%" mkdir "%DEST%"

rem If an older version is loaded, stop the hosts so the file can be replaced.
if exist "%DEST%\DxfPreview.dll" (
  regsvr32 /s /u "%DEST%\DxfPreview.dll"
  taskkill /f /im prevhost.exe >nul 2>&1
  taskkill /f /im dllhost.exe >nul 2>&1
  timeout /t 1 /nobreak >nul
)
copy /y "%SRC%" "%DEST%\DxfPreview.dll" >nul || (
  echo [!] Could not copy the DLL. Close Explorer windows and try again.
  pause
  exit /b 1
)
copy /y "%~dp0uninstall.bat" "%DEST%\uninstall.bat" >nul 2>&1
regsvr32 /s "%DEST%\DxfPreview.dll" || (
  echo [!] Registration failed.
  pause
  exit /b 1
)
echo Installed to %DEST%
echo.
echo  - Thumbnails: View ^> Large icons in Explorer.
echo  - Preview pane: View ^> Preview pane (Alt+P), then click a .dxf file.
echo  - If old icons are still shown, run clear-thumbnail-cache.bat
pause
endlocal
