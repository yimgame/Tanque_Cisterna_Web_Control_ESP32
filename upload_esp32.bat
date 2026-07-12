@echo off
setlocal ENABLEEXTENSIONS

cd /d "%~dp0"

set "PORT=%~1"
if not defined PORT set "PORT=COM6"

set "FQBN=esp32:esp32:esp32"
set "SKETCH_DIR=%~dp0sketch"

set "SCOOP_SHIMS=%USERPROFILE%\scoop\shims"
if exist "%SCOOP_SHIMS%\arduino-cli.exe" (
  set "PATH=%SCOOP_SHIMS%;%PATH%"
)

where arduino-cli >nul 2>&1
if errorlevel 1 (
  echo ERROR: arduino-cli no esta disponible en PATH.
  echo Instala arduino-cli o agrega su ruta al PATH y vuelve a intentar.
  pause
  exit /b 1
)

echo ==============================
echo  Compilando sketch
echo  Puerto: %PORT%
echo  FQBN:   %FQBN%
echo ==============================
arduino-cli compile --fqbn %FQBN% "%SKETCH_DIR%"
if errorlevel 1 (
  echo.
  echo ERROR: fallo la compilacion.
  pause
  exit /b 1
)

echo.
echo ==============================
echo  Subiendo a %PORT%
echo ==============================
arduino-cli upload -p %PORT% --fqbn %FQBN% "%SKETCH_DIR%"
if errorlevel 1 (
  echo.
  echo ERROR: fallo la carga a la placa.
  pause
  exit /b 1
)

echo.
echo Listo. Sketch cargado correctamente en %PORT%.
pause