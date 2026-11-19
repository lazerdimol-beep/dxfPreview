@echo off
rem Removes the DXF thumbnail + preview extension (needs administrator rights).
setlocal
net session >nul 2>&1
if %errorlevel% neq 0 (
  powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
  exit /b
)
set "DEST=%ProgramFiles%\DxfPreview"
if exist "%DEST%\DxfPreview.dll" regsvr32 /s /u "%DEST%\DxfPreview.dll"
taskkill /f /im prevhost.exe >nul 2>&1
taskkill /f /im dllhost.exe >nul 2>&1
timeout /t 1 /nobreak >nul
del /f /q "%DEST%\DxfPreview.dll" >nul 2>&1
if exist "%DEST%\DxfPreview.dll" (
  echo The DLL is still in use; it will be removed after you restart Windows.
  move /y "%DEST%\DxfPreview.dll" "%TEMP%\DxfPreview.old.dll" >nul 2>&1
)
echo Uninstalled.
pause
endlocal
