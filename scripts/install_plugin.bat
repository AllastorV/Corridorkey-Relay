@echo off
net session >nul 2>&1
if errorlevel 1 (
  powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
  exit /b
)
set "SRC=%~dp0..\build\CorridorKey2.ofx.bundle"
if exist "%~dp0CorridorKey2.ofx.bundle\Contents\Win64\CorridorKey2.ofx" set "SRC=%~dp0CorridorKey2.ofx.bundle"
if not exist "%SRC%\Contents\Win64\CorridorKey2.ofx" (
  echo Plugin build not found. Run build.bat first.
  pause
  exit /b 1
)
set "DST=%CommonProgramFiles%\OFX\Plugins\CorridorKey2.ofx.bundle"
robocopy "%SRC%" "%DST%" /MIR /NFL /NDL /NJH /NJS
if errorlevel 8 (
  echo Copy failed. Close DaVinci Resolve and try again.
  pause
  exit /b 1
)
echo Installed: %DST%
echo Restart DaVinci Resolve. Effect: OpenFX ^> Keying ^> CorridorKey Relay
pause
