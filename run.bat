@echo off
REM Windows batch script to run GPURayTraceExample

echo Running GPURayTraceExample from Windows...
echo.

REM Change to the project directory and run via WSL
wsl bash -c "cd '/mnt/d/Shared With Desktop/AI/MatterEngine2/GPURayTraceExample' && chmod +x run.sh && ./run.sh"

echo.
echo Application finished.
pause 