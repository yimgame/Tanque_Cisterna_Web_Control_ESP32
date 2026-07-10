@echo off
setlocal ENABLEDELAYEDEXPANSION

cd /d "%~dp0"
set "EXIT_CODE=0"
set "TARGET_BRANCH=main"
set "TARGET_REMOTE=origin"

git rev-parse --is-inside-work-tree >nul 2>&1
if errorlevel 1 goto :not_repo

echo [1/6] Estado actual...
git status
if errorlevel 1 goto :error

echo.
echo [2/6] Agregando cambios (git add .)...
git add .
if errorlevel 1 goto :error

echo.
echo [3/6] Asegurando rama %TARGET_BRANCH%...
git branch -M %TARGET_BRANCH%
if errorlevel 1 goto :error

echo.
set "COMMIT_MSG="
set /p COMMIT_MSG=[4/6] Mensaje del commit: 
if not defined COMMIT_MSG (
  echo ERROR: Debes ingresar un mensaje de commit.
  set "EXIT_CODE=1"
  goto :end
)

echo.
echo [5/6] Creando commit...
git commit -m "%COMMIT_MSG%"
if errorlevel 1 goto :error

echo.
echo [6/6] Enviando a %TARGET_REMOTE%/%TARGET_BRANCH%...
git push -u %TARGET_REMOTE% %TARGET_BRANCH%
if errorlevel 1 goto :error

goto :summary

:error
set "EXIT_CODE=%ERRORLEVEL%"
echo.
echo ERROR: Fallo un comando git. Codigo !EXIT_CODE!.
echo Revisa los mensajes de arriba.
goto :summary

:not_repo
set "EXIT_CODE=1"
echo.
echo ERROR: Ejecuta este archivo dentro de un repositorio Git.
goto :end

:summary
echo.
echo ===== RESUMEN =====
git status -sb
echo.
git log -1 --oneline
echo ===================

:end
echo.
pause
exit /b %EXIT_CODE%