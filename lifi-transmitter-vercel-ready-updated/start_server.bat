@echo off
title Li-Fi Transmitter Local Server
echo ===================================================
echo   Li-Fi Data Transmitter Pro - Local Web Server
echo ===================================================
echo.
echo Starting local web server on port 8080...
echo.
echo Open in your PC browser:
echo    http://localhost:8080
echo.
echo Open on your smartphone (connected to same Wi-Fi):
echo.
for /f "tokens=2 delims=:" %%a in ('ipconfig ^| findstr /c:"IPv4 Address"') do (
    echo    http:%%a:8080
)
echo.
echo Press Ctrl+C in this window to stop the server anytime.
echo.
python -m http.server 8080
pause
