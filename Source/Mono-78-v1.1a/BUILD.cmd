@echo off
setlocal
cd /d "%~dp0"
set "MONO_VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%MONO_VSWHERE%" goto missing
for /f "usebackq tokens=*" %%i in (`"%MONO_VSWHERE%" -latest -products * -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe`) do set "MONO_MSBUILD=%%i"
if not defined MONO_MSBUILD goto missing
"%MONO_MSBUILD%" Mono-78.sln /p:Configuration=Release /p:Platform=x64 /m
if errorlevel 1 goto failed
echo Build complete. Find Mono-78.exe in the Release output folder.
pause
exit /b 0
:missing
echo Visual Studio with C++ tools is required. Open Mono-78.sln and build Release/x64.
:failed
pause
exit /b 1
