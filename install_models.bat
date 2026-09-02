@echo off
setlocal

set "SCRIPT_DIR=%~dp0"
set "MODEL_ROOT=%~1"
if "%MODEL_ROOT%"=="" set "MODEL_ROOT=%SCRIPT_DIR%external-models"

if not exist "%MODEL_ROOT%" (
  echo Model directory does not exist:
  echo   %MODEL_ROOT%
  exit /b 1
)

setx RIFEOFX_MODELS_ROOT "%MODEL_ROOT%" >nul
if errorlevel 1 (
  echo Could not set the user environment variable RIFEOFX_MODELS_ROOT.
  echo For the current PowerShell session, run:
  echo   $env:RIFEOFX_MODELS_ROOT = "%MODEL_ROOT%"
  exit /b 1
)

echo RIFEOFX_MODELS_ROOT is now set for newly launched applications:
echo   %MODEL_ROOT%
echo Restart DaVinci Resolve before testing.
echo Expected file for RIFE 4.25:
echo   %MODEL_ROOT%\rife-v4.25\flownet.param
echo   %MODEL_ROOT%\rife-v4.25\flownet.bin
exit /b 0
