# PowerShell script to run GPURayTraceExample

Write-Host "Running GPURayTraceExample from Windows PowerShell..." -ForegroundColor Green
Write-Host ""

# Change to the project directory and run via WSL
try {
    wsl bash -c "cd '/mnt/d/Shared With Desktop/AI/MatterEngine2/GPURayTraceExample' && chmod +x run.sh && ./run.sh"
}
catch {
    Write-Host "Error running application: $_" -ForegroundColor Red
    Write-Host ""
    Write-Host "Make sure WSL is installed and the project is built." -ForegroundColor Yellow
}

Write-Host ""
Write-Host "Application finished." -ForegroundColor Green
Read-Host "Press Enter to continue" 