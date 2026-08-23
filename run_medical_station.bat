@echo off
title ICU Assistive Medical Station
cd /d "%~dp0"
echo =========================================================
echo    🏥 Starting ICU Assistive Medical Station...
echo =========================================================
echo Installing required Python packages (flask, pyserial)...
pip install -r requirements.txt
echo.
echo Launching Medical Station Dashboard on http://localhost:5000 ...
start http://localhost:5000
python medical_station.py
pause
