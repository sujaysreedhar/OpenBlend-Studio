param([string]$Destination = "$env:LOCALAPPDATA/Programs/ProjectionStudio", [switch]$NoShortcut)
$ErrorActionPreference='Stop'
$taskSource=[IO.Path]::GetFullPath($PSScriptRoot)
if (!(Test-Path (Join-Path $taskSource 'projection-studio.exe'))) { throw 'Run the install script from the extracted portable package.' }
$taskDestination=[IO.Path]::GetFullPath($Destination)
if ($taskDestination.StartsWith($taskSource,[StringComparison]::OrdinalIgnoreCase) -or $taskSource.StartsWith($taskDestination,[StringComparison]::OrdinalIgnoreCase)) { throw 'Choose an installation directory outside the package tree.' }
if ((Test-Path $taskDestination) -and (Get-ChildItem -LiteralPath $taskDestination -Force | Select-Object -First 1) -and !(Test-Path (Join-Path $taskDestination '.projection-studio-install.json'))) { throw 'The destination contains unrelated files. Choose an empty directory.' }
New-Item -ItemType Directory -Force -Path $taskDestination | Out-Null
$taskFiles=@()
foreach($taskFile in Get-ChildItem -LiteralPath $taskSource -Recurse -File) {
    $taskRelative=$taskFile.FullName.Substring($taskSource.TrimEnd('\','/').Length+1)
    if($taskRelative -eq '.projection-studio-install.json'){continue}
    $taskTarget=Join-Path $taskDestination $taskRelative
    New-Item -ItemType Directory -Force -Path (Split-Path $taskTarget -Parent) | Out-Null
    Copy-Item -LiteralPath $taskFile.FullName -Destination $taskTarget -Force
    $taskFiles+=$taskRelative
}
$taskShortcut=$null
if(!$NoShortcut){
    $taskShortcut=Join-Path ([Environment]::GetFolderPath('Desktop')) 'Projection Studio.lnk'
    $taskShell=New-Object -ComObject WScript.Shell
    $taskLink=$taskShell.CreateShortcut($taskShortcut)
    $taskLink.TargetPath=Join-Path $taskDestination 'projection-studio.exe';$taskLink.WorkingDirectory=$taskDestination;$taskLink.Save()
}
@{product='ProjectionStudio';version='0.4.0';files=$taskFiles;shortcut=$taskShortcut}|ConvertTo-Json -Depth 4|Set-Content -LiteralPath (Join-Path $taskDestination '.projection-studio-install.json') -Encoding utf8
Write-Host "Installed to $taskDestination"
