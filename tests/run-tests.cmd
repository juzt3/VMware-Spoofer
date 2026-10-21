@echo off
setlocal
cd /d "%~dp0.."
set VMSPOOF_PROFILES=%CD%\profiles
call build.cmd
if errorlevel 1 exit /b 1
build\tests.exe
exit /b %errorlevel%
