@echo off
setlocal
set VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat
if defined VCINSTALLDIR if exist "%VCINSTALLDIR%\Auxiliary\Build\vcvars64.bat" set VCVARS=%VCINSTALLDIR%\Auxiliary\Build\vcvars64.bat
if not exist "%VCVARS%" (
  echo error: no se encontro vcvars64.bat
  exit /b 1
)
call "%VCVARS%" >nul
if errorlevel 1 exit /b 1

if not exist build\obj\common mkdir build\obj\common
if not exist build\obj\gui mkdir build\obj\gui
if not exist build\obj\tests mkdir build\obj\tests

set DEFS=/DZ7_ST /D_7ZIP_ST /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX
set CFLAGS=/nologo /c /O2 /W3 /utf-8 /MT /Ithird_party\lzma %DEFS%
set CXXFLAGS=%CFLAGS% /std:c++20 /EHsc

echo [1/4] core + LZMA
cl %CXXFLAGS% /Fo:build\obj\common\ src\core.cpp
if errorlevel 1 exit /b 1
cl %CFLAGS% /Fo:build\obj\common\ third_party\lzma\*.c
if errorlevel 1 exit /b 1

echo [2/4] GUI
cl %CXXFLAGS% /Fo:build\obj\gui\ src\gui.cpp
if errorlevel 1 exit /b 1
cl /nologo /Fe:build\vmspoof-gui.exe build\obj\gui\gui.obj build\obj\common\*.obj /link /SUBSYSTEM:WINDOWS /ENTRY:wWinMainCRTStartup bcrypt.lib advapi32.lib gdiplus.lib dwmapi.lib comdlg32.lib comctl32.lib uxtheme.lib shell32.lib ole32.lib user32.lib gdi32.lib
if errorlevel 1 exit /b 1
if exist src\gui.manifest (
  mt.exe -nologo -manifest src\gui.manifest -outputresource:"build\vmspoof-gui.exe;#1"
  if errorlevel 1 exit /b 1
)

echo [3/4] tests
cl %CXXFLAGS% /Fo:build\obj\tests\ tests\test_core.cpp
if errorlevel 1 exit /b 1
cl /nologo /Fe:build\tests.exe build\obj\tests\test_core.obj build\obj\common\*.obj /link bcrypt.lib advapi32.lib
if errorlevel 1 exit /b 1

echo [4/4] release
if exist release rmdir /s /q release
mkdir release
copy /y build\vmspoof-gui.exe release\ >nul
xcopy /e /i /y profiles release\profiles >nul
if exist "back up" xcopy /e /i /y "back up" "release\back up" >nul
for /d %%d in (oem5.inf_amd64_*) do xcopy /e /i /y "%%d" "release\%%~nxd" >nul

if exist profiles (
  if not exist build\profiles mkdir build\profiles
  xcopy /e /i /y profiles build\profiles >nul
)
echo OK
echo   build\vmspoof-gui.exe
echo   build\tests.exe
echo   release\ (vmspoof-gui.exe, profiles, inputs)
