$ErrorActionPreference='Stop'
$repo=Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Add-Type -Path @((Join-Path $repo 'launcher/Core.cs'), (Join-Path $repo 'launcher/RuntimeSetup.cs')) -ReferencedAssemblies @(
 'System.dll', 'System.Core.dll', 'System.Web.Extensions.dll',
 'System.IO.Compression.dll', 'System.IO.Compression.FileSystem.dll'
)
Add-Type -AssemblyName System.IO.Compression.FileSystem
$root=Join-Path $repo ('tmp/progress-check-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory $root | Out-Null
$bytes=New-Object byte[] 1048576
(New-Object Random 42).NextBytes($bytes)
$input=New-Object IO.MemoryStream(,$bytes)
$output=New-Object IO.MemoryStream
$seen=New-Object 'System.Collections.Generic.List[long]'
[Burn3Launcher.Core]::CopyBounded($input,$output,$bytes.Length,[Action[long]]{param($n) $seen.Add($n)})
if ($seen[0] -ne 0 -or $seen[$seen.Count-1] -ne $bytes.Length -or $seen.Count -lt 4) {throw 'Download byte progress missing'}
for($i=1;$i -lt $seen.Count;$i++){if($seen[$i] -lt $seen[$i-1]){throw 'Download progress went backwards'}}
$source=Join-Path $root 'source';New-Item -ItemType Directory $source | Out-Null
[IO.File]::WriteAllBytes((Join-Path $source 'payload.bin'),$bytes)
Copy-Item (Join-Path $repo 'build-launcher/Burnout3Launcher.exe') (Join-Path $source 'Burnout3Launcher.exe')
[IO.File]::WriteAllText((Join-Path $source 'VERSION'),'0.0.4')
$zip=Join-Path $root 'update.zip';[IO.Compression.ZipFile]::CreateFromDirectory($source,$zip)
$p=New-Object Burn3Launcher.Package
$p.version='0.0.4';$p.url='https://uwucammie.com/burn3/downloads/test.zip';$p.bytes=(Get-Item $zip).Length;$p.sha256=[Burn3Launcher.Core]::Hash($zip)
$events=New-Object 'System.Collections.Generic.List[Burn3Launcher.UpdateProgress]'
$callback=[Action[Burn3Launcher.UpdateProgress]]{param($event) $events.Add($event)}
$null=[Burn3Launcher.Core]::InstallArchive((Join-Path $root 'install'),'launcher',$p,$zip,$callback)
foreach($phase in @('Verifying','Installing')){
 $rows=@($events | Where-Object Stage -eq $phase)
 if($rows.Count -lt 2 -or $rows[0].Completed -ne 0 -or $rows[-1].Completed -ne $rows[-1].Total){throw "Incomplete $phase progress"}
}
if($events[$events.Count-1].Stage -ne 'Complete'){throw 'Completion missing'}
$p.sha256='0'*64;$events.Clear();$rejected=$false
try{[Burn3Launcher.Core]::InstallArchive((Join-Path $root 'bad'),'launcher',$p,$zip,$callback)}catch{$rejected=$true}
if(-not $rejected -or @($events | Where-Object Stage -eq 'Complete').Count -ne 0){throw 'Bad checksum reported success'}
$logs=Join-Path $root 'logs';New-Item -ItemType Directory $logs | Out-Null
$openLog=[Burn3Launcher.RuntimeSetup].GetMethod('OpenSetupLog',[Reflection.BindingFlags]'Static,NonPublic')
foreach($message in @('First setup: 83 compiled','Updated setup: 83 reused','Latest setup: linking')){
 $log=$openLog.Invoke($null,[object[]]@([string]$logs))
 try{$log.WriteLine($message)}finally{$log.Dispose()}
}
$history=@(Get-ChildItem -LiteralPath $logs -Filter 'setup-*.log')
if($history.Count -ne 2){throw 'Setup history was overwritten'}
$contents=@($history | ForEach-Object { [IO.File]::ReadAllText($_.FullName).Trim() })
if($contents -notcontains 'First setup: 83 compiled' -or $contents -notcontains 'Updated setup: 83 reused'){throw 'Earlier setup output was lost'}
if([IO.File]::ReadAllText((Join-Path $logs 'setup.log')).Trim() -ne 'Latest setup: linking'){throw 'Current setup.log no longer contains latest output'}
$program=[IO.File]::ReadAllText((Join-Path $repo 'launcher/Program.cs'))
if(-not $program.Contains('Checking installed game files...') -or $program.Contains('First-time setup can take several minutes...')){throw 'Setup check misleadingly claims first-time compilation'}
Write-Output 'PASS: byte progress, verification/extraction completion, checksum failure, setup log history, and setup check wording'
