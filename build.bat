@echo off
setlocal
set "VSINSTALLER=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
set "PATH=%VSINSTALLER%;%PATH%"
if not exist "%VSINSTALLER%\vswhere.exe" (
  echo Visual Studio 2022 or Build Tools with the C++ workload is required.
  exit /b 1
)
for /f "usebackq delims=" %%i in (`vswhere.exe -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (
  echo Visual Studio C++ tools not found.
  exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release || exit /b 1
cmake --build build || exit /b 1
echo Built: %~dp0build\CorridorKey2.ofx.bundle
