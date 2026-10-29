param([string]$InstallRoot)
Start-Process -FilePath (Join-Path $PSScriptRoot "Burnout3Launcher.exe") -ArgumentList ('--root "' + $InstallRoot + '"')
