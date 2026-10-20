[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$taskProject = Split-Path -Parent $PSScriptRoot
$taskSpec = Get-Content -LiteralPath (Join-Path $taskProject 'buildspec.json') -Raw | ConvertFrom-Json
$taskStaging = Join-Path $taskProject 'release/package'
$taskDist = Join-Path $taskProject 'dist'
$taskPluginBin = Join-Path $taskStaging 'OBS-Seeker/bin/64bit'
New-Item -ItemType Directory -Path $taskPluginBin -Force | Out-Null
New-Item -ItemType Directory -Path $taskDist -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $taskProject 'release/install/OBS-Seeker/bin/64bit/OBS-Seeker.dll') -Destination $taskPluginBin -Force
Copy-Item -LiteralPath (Join-Path $taskProject 'release/install/OBS-Seeker/bin/64bit/OBS-Seeker-present.exe') -Destination $taskPluginBin -Force
Copy-Item -LiteralPath (Join-Path $taskProject 'scripts/InstallPlugin.bat') -Destination $taskStaging -Force
Set-Content -LiteralPath (Join-Path $taskStaging 'README.MD') -Value 'Copy `OBS-Seeker` to `C:\ProgramData\obs-studio\plugins\`.' -Encoding utf8
Copy-Item -LiteralPath (Join-Path $taskProject 'LICENSE') -Destination $taskStaging -Force
$taskZip = Join-Path $taskDist ("OBS-Seeker-" + $taskSpec.version + '-windows-x64.zip')
$taskPackageFiles = @('OBS-Seeker', 'InstallPlugin.bat', 'README.MD', 'LICENSE') | ForEach-Object { Join-Path $taskStaging $_ }
Compress-Archive -Path $taskPackageFiles -DestinationPath $taskZip -Force
$taskHash = (Get-FileHash -LiteralPath $taskZip -Algorithm SHA256).Hash.ToLowerInvariant()
Set-Content -LiteralPath (Join-Path $taskDist 'SHA256SUMS.txt') -Value ($taskHash + '  ' + (Split-Path -Leaf $taskZip)) -Encoding utf8
Write-Output ('Created ' + $taskZip)
