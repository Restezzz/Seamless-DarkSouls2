@echo off
title DS2 Seamless Co-op - stop server
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0SeamlessServer\stop_server.ps1"
timeout /t 4 >nul
