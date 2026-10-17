@echo off
setlocal EnableExtensions DisableDelayedExpansion
set "seeker_plugin_root=%ProgramData%\obs-studio\plugins"
if not "%~1"=="" set "seeker_plugin_root=%~f1"
set "seeker_source=%~dp0OBS-Seeker\bin\64bit\OBS-Seeker.dll"
set "seeker_presenter=%~dp0OBS-Seeker\bin\64bit\OBS-Seeker-present.exe"
set "seeker_destination=%seeker_plugin_root%\OBS-Seeker\bin\64bit"

"%SystemRoot%\System32\tasklist.exe" /FI "IMAGENAME eq obs64.exe" /NH 2>nul | "%SystemRoot%\System32\find.exe" /I "obs64.exe" >nul
if not errorlevel 1 (
    echo Please close OBS Studio before installing or updating.
    goto failed
)
if not exist "%seeker_source%" (
    echo Extract the plugin ZIP first, then run this installer.
    goto failed
)
if not exist "%seeker_presenter%" (
    echo Extract the complete plugin ZIP first, then run this installer.
    goto failed
)
if not exist "%seeker_destination%\" mkdir "%seeker_destination%" 2>nul
if not exist "%seeker_destination%\" (
    echo Cannot create the plugin folder. Check folder permissions.
    goto failed
)
copy /B /Y "%seeker_source%" "%seeker_destination%\OBS-Seeker.dll" >nul
if errorlevel 1 (
    echo Installation failed. Check folder permissions and close OBS.
    goto failed
)
copy /B /Y "%seeker_presenter%" "%seeker_destination%\OBS-Seeker-present.exe" >nul
if errorlevel 1 (
    echo Installation failed. Check folder permissions and close OBS.
    goto failed
)
echo Installed to "%seeker_destination%"
echo Start OBS, open the Seeker dock, and add folders in Settings.
if /I not "%~2"=="/quiet" powershell.exe -NoLogo -NoProfile -Command "for ($seconds = 5; $seconds -gt 0; $seconds--) { [Console]::Write(('{0}Closing in {1}s... Press any key to exit. ' -f [char]13, $seconds)); for ($tick = 0; $tick -lt 10; $tick++) { if (-not [Console]::IsInputRedirected -and [Console]::KeyAvailable) { [void][Console]::ReadKey($true); [Console]::WriteLine(); exit 0 }; Start-Sleep -Milliseconds 100 } }; [Console]::WriteLine()"
exit /b 0

:failed
if /I not "%~2"=="/quiet" pause
exit /b 1
