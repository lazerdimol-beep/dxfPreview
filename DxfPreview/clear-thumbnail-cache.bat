@echo off
rem Clears the Explorer thumbnail cache so .dxf thumbnails are regenerated.
echo Explorer will restart for a moment...
taskkill /f /im explorer.exe >nul 2>&1
timeout /t 1 /nobreak >nul
del /f /q /a "%LocalAppData%\Microsoft\Windows\Explorer\thumbcache_*.db" >nul 2>&1
start explorer.exe
echo Done.
