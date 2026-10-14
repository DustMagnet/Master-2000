@echo off
setlocal
cd /d "%~dp0"
where msbuild >nul 2>nul
if errorlevel 1 (echo Run from a Visual Studio Developer Command Prompt.& exit /b 1)
set "DIST=%CD%\artifacts\Master-2000-v0.3.3"
if exist "%DIST%" (echo Output already exists. Rename or remove that generated folder before rebuilding.& exit /b 1)
msbuild "Master-2000-App\Master-2000.sln" /m /t:Restore;Build /p:Configuration=Release /p:Platform=x64
if errorlevel 1 exit /b 1
msbuild "Master-2000-App\Vinyl2000.App\Vinyl2000.App.csproj" /m /t:Publish /p:Configuration=Release /p:Platform=x64 /p:RuntimeIdentifier=win-x64 /p:PublishSingleFile=false /p:SelfContained=true /p:WindowsAppSDKSelfContained=true /p:PublishDir="%DIST%\"
if errorlevel 1 exit /b 1
copy "Master-2000-App\bin\Release\Master2000Engine.exe" "%DIST%\Master2000Engine.exe" >nul
if errorlevel 1 exit /b 1
copy LICENSE "%DIST%\LICENSE.txt" >nul
copy THIRD-PARTY-NOTICES.md "%DIST%\THIRD-PARTY-NOTICES.md" >nul
if not exist "%DIST%\Master-2000.exe" exit /b 1
if not exist "%DIST%\Master2000Engine.exe" exit /b 1
echo Portable candidate created. Complete RELEASE-CHECKLIST.md before publication.
exit /b 0
