param([string]$Destination=$PSScriptRoot)
$ErrorActionPreference='Stop'
$taskRoot=[IO.Path]::GetFullPath($Destination).TrimEnd('\','/')
$taskMarker=Join-Path $taskRoot '.projection-studio-install.json'
if(!(Test-Path -LiteralPath $taskMarker)){throw 'Not a marked Projection Studio installation.'}
$taskManifest=Get-Content -LiteralPath $taskMarker -Raw|ConvertFrom-Json
if($taskManifest.product -ne 'ProjectionStudio'){throw 'Invalid installation marker.'}
# Resolve and check every target before removing any installed files.
$taskTargets=@()
foreach($taskRelative in $taskManifest.files){
    $taskTarget=[IO.Path]::GetFullPath((Join-Path $taskRoot $taskRelative))
    if(!$taskTarget.StartsWith($taskRoot+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)){throw 'Install manifest path escapes installation directory.'}
    $taskTargets+=$taskTarget
}
foreach($taskTarget in $taskTargets){if(Test-Path -LiteralPath $taskTarget -PathType Leaf){Remove-Item -LiteralPath $taskTarget -Force}}
if($taskManifest.shortcut){
    $taskExpected=Join-Path ([Environment]::GetFolderPath('Desktop')) 'Projection Studio.lnk'
    if($taskManifest.shortcut -eq $taskExpected -and (Test-Path -LiteralPath $taskExpected)){
        $taskShell=New-Object -ComObject WScript.Shell
        if($taskShell.CreateShortcut($taskExpected).TargetPath -eq (Join-Path $taskRoot 'projection-studio.exe')){Remove-Item -LiteralPath $taskExpected}
    }
}
Remove-Item -LiteralPath $taskMarker
# Empty directories only: preserve any user-added projects or assets.
Get-ChildItem -LiteralPath $taskRoot -Directory -Recurse | Sort-Object { $_.FullName.Length } -Descending | ForEach-Object { if(!(Get-ChildItem -LiteralPath $_.FullName -Force | Select-Object -First 1)){Remove-Item -LiteralPath $_.FullName} }
if(!(Get-ChildItem -LiteralPath $taskRoot -Force | Select-Object -First 1)){Remove-Item -LiteralPath $taskRoot}
Write-Host 'Projection Studio removed. User-added files were preserved.'
