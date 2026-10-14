@echo off
setlocal
cd /d "%~dp0"

echo ============================================================
echo   Master-2000 v0.3.3
echo   Release x64 TRUE single-file publisher
echo ============================================================
echo.

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo ERROR: vswhere.exe not found. Install Visual Studio 2026 first.
  pause
  exit /b 1
)

for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.Component.MSBuild -property installationPath`) do set "VSROOT=%%i"
if not defined VSROOT (
  echo ERROR: No Visual Studio installation with MSBuild was found.
  pause
  exit /b 1
)

set "MSBUILD=%VSROOT%\MSBuild\Current\Bin\MSBuild.exe"
if not exist "%MSBUILD%" (
  echo ERROR: MSBuild.exe not found at:
  echo %MSBUILD%
  pause
  exit /b 1
)

set "DIST=%CD%\Master-2000"
if exist "%DIST%" rmdir /s /q "%DIST%"
mkdir "%DIST%"

echo [1/2] Building native engine and GUI in Release x64...
"%MSBUILD%" "Master-2000.sln" /m /t:Restore;Build /p:Configuration=Release /p:Platform=x64
if errorlevel 1 goto :fail

echo.
echo [2/2] Publishing one self-contained EXE...
"%MSBUILD%" "Vinyl2000.App\Vinyl2000.App.csproj" /m /t:Publish /p:Configuration=Release /p:Platform=x64 /p:RuntimeIdentifier=win-x64 /p:PublishSingleFile=true /p:SelfContained=true /p:WindowsAppSDKSelfContained=true /p:IncludeNativeLibrariesForSelfExtract=true /p:IncludeAllContentForSelfExtract=true /p:PublishDir="%DIST%"
if errorlevel 1 goto :fail

echo.
echo Checking true single-file output...
if not exist "%DIST%\Master-2000.exe" (
  echo ERROR: Master-2000.exe was not created.
  goto :fail
)

for /f %%N in ('dir /b /a-d "%DIST%" ^| find /c /v ""') do set "FILECOUNT=%%N"
if not "%FILECOUNT%"=="1" (
  echo ERROR: Publish produced %FILECOUNT% files instead of one.
  echo A true standalone build must contain ONLY:
  echo   Master-2000.exe
  echo.
  dir /b "%DIST%"
  goto :fail
)

echo.
echo ============================================================
copy "%~dp0..\LICENSE" "%DIST%\LICENSE.txt" >nul
if errorlevel 1 goto :fail
copy "%~dp0..\THIRD-PARTY-NOTICES.md" "%DIST%\THIRD-PARTY-NOTICES.md" >nul
if errorlevel 1 goto :fail
echo SUCCESS - executable and license notices prepared

echo Look in:
echo   %DIST%
echo.
echo The distributable file should be:
echo   Master-2000.exe plus license notices

echo ============================================================
explorer "%DIST%"
pause
exit /b 0

:fail
echo.
echo ============================================================
echo BUILD FAILED - send the last part of this window to ChatGPT.
echo ============================================================
pause
exit /b 1
