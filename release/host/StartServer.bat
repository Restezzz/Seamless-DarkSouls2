@echo off
title DS2 Seamless Co-op - server
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0SeamlessServer\start_server.ps1"
echo.
pause
