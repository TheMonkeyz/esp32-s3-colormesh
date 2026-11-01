@echo off
cd /d "%~dp0"
rem Opens the flash helper in its own (normal-size) window so you can watch progress. Close it to stop.
rem Its files live in <repo>\.devloop (docs/PROTOCOL.md); agents drive it with tools\devloop\devloop.py.
start "ESP flash helper" powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0flash_helper.ps1"
