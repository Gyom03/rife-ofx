@echo off
setlocal

set "SCRIPT_DIR=%~dp0"
set "BUNDLE=%SCRIPT_DIR%RifeFrameInterpolator.ofx.bundle"
if not exist "%BUNDLE%" set "BUNDLE=%SCRIPT_DIR%build-rife\RifeFrameInterpolator.ofx.bundle"
if not exist "%BUNDLE%" set "BUNDLE=%SCRIPT_DIR%build-rife-ai\RifeFrameInterpolator.ofx.bundle"
if not exist "%BUNDLE%" set "BUNDLE=%SCRIPT_DIR%build-release\RifeFrameInterpolator.ofx.bundle"
if not exist "%BUNDLE%" set "BUNDLE=%SCRIPT_DIR%build\RifeFrameInterpolator.ofx.bundle"
set "DEST=C:\Program Files\Common Files\OFX\Plugins\RifeFrameInterpolator.ofx.bundle"

if not exist "%BUNDLE%" (
  echo Bundle not found:
  echo   %SCRIPT_DIR%build-rife-ai\RifeFrameInterpolator.ofx.bundle
  echo   %SCRIPT_DIR%build-rife\RifeFrameInterpolator.ofx.bundle
  echo   %SCRIPT_DIR%build\RifeFrameInterpolator.ofx.bundle
  echo Build it first with: cmake --build build-rife --config Release
  exit /b 1
)

net session >nul 2>&1
if %errorlevel% neq 0 (
  echo Administrator privileges are required to install into:
  echo   C:\Program Files\Common Files\OFX\Plugins\
  echo Right-click this file and choose ^"Run as administrator^".
  exit /b 5
)

if not exist "C:\Program Files\Common Files\OFX\Plugins" (
  mkdir "C:\Program Files\Common Files\OFX\Plugins"
)

robocopy "%BUNDLE%" "%DEST%" /E /NFL /NDL /NJH /NJS /NP
set "COPY_RESULT=%errorlevel%"
if %COPY_RESULT% geq 8 (
  echo Installation failed with robocopy code %COPY_RESULT%.
  exit /b %COPY_RESULT%
)

echo Installed:
echo   %DEST%
echo Restart DaVinci Resolve, then search for ^"RIFE Frame Interpolator^" in OpenFX.
exit /b 0
