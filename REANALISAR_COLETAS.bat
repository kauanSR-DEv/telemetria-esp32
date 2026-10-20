@echo off
setlocal
cd /d "%~dp0"
if exist ".venv\Scripts\python.exe" (
  ".venv\Scripts\python.exe" ferramentas\organizar_logs.py --todas
) else (
  where py >nul 2>nul
  if errorlevel 1 (
    python ferramentas\organizar_logs.py --todas
  ) else (
    py -3 ferramentas\organizar_logs.py --todas
  )
)
echo.
echo Reanalise encerrada. Confira os resumos em resultados\coletas.
pause
