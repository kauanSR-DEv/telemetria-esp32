@echo off
setlocal
cd /d "%~dp0"
where py >nul 2>nul
if errorlevel 1 (
  where python >nul 2>nul
  if errorlevel 1 (
    echo Instale Python 3.10 ou superior em https://www.python.org/downloads/
    echo Na instalacao, marque Add Python to PATH.
    pause
    exit /b 1
  )
  set "TELEMETRIA_PY=python"
) else (
  set "TELEMETRIA_PY=py -3"
)
if not exist ".venv\Scripts\python.exe" (
  %TELEMETRIA_PY% -m venv .venv
  if errorlevel 1 goto falha
)
".venv\Scripts\python.exe" -c "import serial" >nul 2>nul
if errorlevel 1 (
  echo Preparando o coletor pela primeira vez. Aguarde...
  ".venv\Scripts\python.exe" -m pip install -r requirements.txt
  if errorlevel 1 goto falha
)
".venv\Scripts\python.exe" ferramentas\coletar.py
if errorlevel 1 goto falha
exit /b 0
:falha
echo Nao foi possivel abrir o coletor. Confira as mensagens acima.
pause
exit /b 1
