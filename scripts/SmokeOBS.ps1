[CmdletBinding()]
param([switch] $Baseline, [ValidateSet('zh-CN', 'en-US')] [string] $Locale = 'zh-CN')
$ErrorActionPreference = 'Stop'
$taskProject = Split-Path -Parent $PSScriptRoot
$taskVersion = (Get-Content -LiteralPath (Join-Path $taskProject 'buildspec.json') -Raw | ConvertFrom-Json).version
$taskRuntime = Join-Path $taskProject '.runtime'
$taskOBS = Join-Path $taskRuntime $(if ($Baseline) { 'obs-baseline' } elseif ($Locale -eq 'en-US') { 'obs-en-US' } else { 'obs' })
$taskTitle = if ($Locale -eq 'en-US') { 'Seeker' } else { '寻求者' }
$taskArchive = Join-Path $taskRuntime 'OBS-Studio-32.0.4-Windows-x64.zip'
New-Item -ItemType Directory -Path $taskRuntime -Force | Out-Null
if (-not (Test-Path -LiteralPath $taskArchive -PathType Leaf)) {
    Invoke-WebRequest -Uri 'https://github.com/obsproject/obs-studio/releases/download/32.0.4/OBS-Studio-32.0.4-Windows-x64.zip' -OutFile $taskArchive
}
if ((Get-FileHash -LiteralPath $taskArchive -Algorithm SHA256).Hash.ToLowerInvariant() -ne 'fd367a7f156319426f1a600720c1288ad0011da168dff41e0f94dbb18cc1dc0f') {
    throw 'OBS runtime ZIP checksum mismatch.'
}
Expand-Archive -LiteralPath $taskArchive -DestinationPath $taskOBS -Force
$taskPackage = Get-ChildItem -LiteralPath (Join-Path $taskRuntime 'artifact') -Filter 'OBS-Seeker-*-windows-x64.zip' | Select-Object -First 1
if (-not $taskPackage) { throw 'Compiled plugin package was not downloaded.' }
$taskUnpacked = Join-Path $taskRuntime 'plugin'
Expand-Archive -LiteralPath $taskPackage.FullName -DestinationPath $taskUnpacked -Force
$taskPluginDll = Join-Path $taskOBS 'obs-plugins/64bit/OBS-Seeker.dll'
if (-not $Baseline) {
    $taskInstallRoot = Join-Path $taskRuntime 'install check/plugins'
    $taskInstaller = Join-Path $taskUnpacked 'InstallPlugin.bat'
    & $taskInstaller $taskInstallRoot /quiet
    if ($LASTEXITCODE -ne 0) { throw 'BAT installer failed.' }
    & $taskInstaller $taskInstallRoot /quiet
    if ($LASTEXITCODE -ne 0) { throw 'Repeated BAT installation failed.' }
    $taskInstalledDll = Join-Path $taskInstallRoot 'OBS-Seeker/bin/64bit/OBS-Seeker.dll'
    if ((Get-FileHash -LiteralPath $taskInstalledDll).Hash -ne (Get-FileHash -LiteralPath (Join-Path $taskUnpacked 'OBS-Seeker/bin/64bit/OBS-Seeker.dll')).Hash) {
        throw 'BAT installer copied a different DLL.'
    }
    Copy-Item -LiteralPath (Join-Path $taskUnpacked 'OBS-Seeker/bin/64bit/OBS-Seeker.dll') -Destination $taskPluginDll -Force
    $taskHelper = Join-Path $taskUnpacked 'OBS-Seeker/bin/64bit/OBS-Seeker-present.exe'
    $taskInstalledHelper = Join-Path $taskInstallRoot 'OBS-Seeker/bin/64bit/OBS-Seeker-present.exe'
    if ((Get-FileHash -LiteralPath $taskHelper).Hash -ne (Get-FileHash -LiteralPath $taskInstalledHelper).Hash) {
        throw 'BAT installer copied a different presentation helper.'
    }
    Copy-Item -LiteralPath $taskHelper -Destination (Split-Path -Parent $taskPluginDll) -Force
}
$taskValidation = Join-Path $taskRuntime $(if ($Baseline) { 'validation-baseline.txt' } elseif ($Locale -eq 'en-US') { 'validation-en-US.txt' } else { 'validation.txt' })
$taskConfig = Join-Path $taskOBS 'config/obs-studio'
New-Item -ItemType Directory -Path $taskConfig -Force | Out-Null
# 测试实例跳过首次使用对话框。
Set-Content -LiteralPath (Join-Path $taskConfig 'user.ini') -Value "[General]`nFirstRun=true`nLanguage=$Locale`n" -Encoding utf8
Set-Content -LiteralPath (Join-Path $taskConfig 'global.ini') -Value "[General]`nLastVersion=536870912`n" -Encoding utf8
$taskExecutable = Join-Path $taskOBS 'bin/64bit/obs64.exe'
$taskExistingLogs = @(Get-ChildItem -LiteralPath (Join-Path $taskConfig 'logs') -Filter '*.txt' -ErrorAction SilentlyContinue).FullName
$taskProcess = Start-Process -FilePath $taskExecutable -ArgumentList '--portable','--multi','--disable-updater' -WorkingDirectory (Split-Path -Parent $taskExecutable) -WindowStyle Hidden -PassThru
$taskLogText = ''
$taskWatch = [System.Diagnostics.Stopwatch]::StartNew()
try {
    while ($taskWatch.Elapsed.TotalSeconds -lt 60) {
        Start-Sleep -Milliseconds 500
        $taskLogs = Get-ChildItem -LiteralPath (Join-Path $taskConfig 'logs') -Filter '*.txt' -ErrorAction SilentlyContinue | Where-Object { $_.FullName -notin $taskExistingLogs } | Sort-Object LastWriteTime -Descending
        if ($taskLogs) { $taskLogText = Get-Content -LiteralPath $taskLogs[0].FullName -Raw }
        if ($taskLogText -match '==== Startup complete' -and ($Baseline -or $taskLogText -match '\[OBS-Seeker\] Search dock registered')) { break }
        if ($taskProcess.HasExited) { break }
    }
    if ($taskLogText -notmatch '==== Startup complete' -or ($Baseline -and $taskLogText -match '\[OBS-Seeker\]') -or (-not $Baseline -and ($taskLogText -notmatch ('\[OBS-Seeker\] Loaded version ' + [regex]::Escape($taskVersion)) -or $taskLogText -notmatch '\[OBS-Seeker\] Search dock registered' -or $taskLogText -notmatch ('\[OBS-Seeker\] UI title: ' + [regex]::Escape($taskTitle)) -or $taskLogText -notmatch 'Search and refresh hotkeys registered' -or $taskLogText -notmatch 'Favorites hotkeys registered'))) {
        Set-Content -LiteralPath $taskValidation -Value 'FAIL: OBS startup validation failed.'
        throw 'Plugin load/dock registration could not be verified; inspect uploaded OBS logs.'
    }
    # 按进程和 OBS 主窗口标题定位，避免关闭同进程的 OBS-Seeker 面板。
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class OBSRuntimeTestWindow {
    private delegate bool EnumCallback(IntPtr window, IntPtr parameter);
    [DllImport("user32.dll")] private static extern bool EnumWindows(EnumCallback callback, IntPtr parameter);
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr window, out uint process);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern int GetWindowText(IntPtr window, StringBuilder text, int length);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern bool PostMessage(IntPtr window, uint message, IntPtr wParam, IntPtr lParam);
    public static bool CloseOwnOBSWindow(int processId) {
        bool sent = false;
        EnumWindows((window, parameter) => {
            uint owner;
            GetWindowThreadProcessId(window, out owner);
            if (owner != (uint)processId) return true;
            var title = new StringBuilder(1024);
            GetWindowText(window, title, title.Capacity);
            if (!title.ToString().StartsWith("OBS ", StringComparison.OrdinalIgnoreCase)) return true;
            Console.WriteLine("Closing OBS main window: " + title);
            sent = PostMessage(window, 0x0010, IntPtr.Zero, IntPtr.Zero);
            return !sent;
        }, IntPtr.Zero);
        return sent;
    }
}
'@
    $taskCloseWatch = [System.Diagnostics.Stopwatch]::StartNew()
    $taskClosed = $false
    while (-not $taskClosed -and $taskCloseWatch.Elapsed.TotalSeconds -lt 10 -and -not $taskProcess.HasExited) {
        $taskClosed = [OBSRuntimeTestWindow]::CloseOwnOBSWindow($taskProcess.Id)
        if (-not $taskClosed) { Start-Sleep -Milliseconds 250 }
    }
    $taskExited = $taskClosed -and $taskProcess.WaitForExit(20000)
    if (-not $taskExited) {
        Set-Content -LiteralPath $taskValidation -Value 'FAIL: OBS did not exit normally before the timeout.'
        throw 'OBS graceful shutdown could not be verified.'
    } elseif ($taskProcess.ExitCode -ne 0) {
        throw ('OBS exited with code ' + $taskProcess.ExitCode)
    } else {
        $taskLogText = Get-Content -LiteralPath $taskLogs[0].FullName -Raw
        $taskLeaks = [regex]::Match($taskLogText, 'Number of memory leaks: (\d+)')
        if (-not $taskLeaks.Success) { throw 'OBS memory report is missing.' }
        $taskResult = "PASS: OBS 32.0.4 exited cleanly; locale=$Locale; baseline=$([bool]$Baseline); memory leaks=$($taskLeaks.Groups[1].Value)."
        Set-Content -LiteralPath $taskValidation -Value $taskResult
        Write-Output $taskResult
    }
} finally {
    if (-not $taskProcess.HasExited) { Stop-Process -Id $taskProcess.Id -Force }
}
