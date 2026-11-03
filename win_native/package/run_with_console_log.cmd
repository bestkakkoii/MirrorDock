@echo off
rem SPDX-License-Identifier: MIT
rem Starts UxPlay with its log printed in this console window (for diagnosis).
rem Extra options can be appended, e.g.:  run_with_console_log.cmd -d
chcp 65001 >nul
cd /d "%~dp0"
uxplay.exe %*
