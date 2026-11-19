@echo off
rem Builds build\DxfPreview.dll (Explorer extension) and build\dxfthumb.exe (test tool).
rem Requires Visual Studio 2019/2022 or "Build Tools for Visual Studio" with the C++ workload.
setlocal
cd /d "%~dp0"

where cl >nul 2>nul
if %errorlevel%==0 goto build

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo [!] Visual Studio not found. Install "Build Tools for Visual Studio" with "Desktop development with C++".
  exit /b 1
)
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH (
  echo [!] C++ build tools not found in Visual Studio. Add the "Desktop development with C++" workload.
  exit /b 1
)
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

:build
if not exist build mkdir build
if not exist build\obj mkdir build\obj
if not exist build\objtool mkdir build\objtool

set CFLAGS=/nologo /O2 /EHsc /std:c++17 /MT /utf-8 /W3 /DUNICODE /D_UNICODE /DNOMINMAX
set LIBS=gdiplus.lib shlwapi.lib ole32.lib oleaut32.lib uuid.lib advapi32.lib user32.lib gdi32.lib shell32.lib

echo Building DxfPreview.dll ...
cl %CFLAGS% /LD src\dxf_core.cpp src\render_gdiplus.cpp src\dll_main.cpp src\thumbnail_provider.cpp src\preview_handler.cpp ^
   /Fo:build\obj\ /Fe:build\DxfPreview.dll /link /DEF:src\DxfPreview.def %LIBS% || exit /b 1

echo Building dxfthumb.exe ...
cl %CFLAGS% tools\dxfthumb.cpp src\dxf_core.cpp src\render_gdiplus.cpp ^
   /Fo:build\objtool\ /Fe:build\dxfthumb.exe /link %LIBS% || exit /b 1

echo.
echo Done: build\DxfPreview.dll, build\dxfthumb.exe
endlocal
