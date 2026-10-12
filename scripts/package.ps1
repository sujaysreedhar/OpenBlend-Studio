param([string]$Build='build/portable',[string]$Qt='.tools/Qt/6.8.3/mingw_64',[string]$OpenCV='.tools/opencv',[switch]$SourceBundle)
$ErrorActionPreference='Stop'
$taskRoot=Split-Path $PSScriptRoot -Parent
Set-Location $taskRoot
$taskPackage=Join-Path $taskRoot 'dist/ProjectionStudio-0.2.0-windows-x64'
New-Item -ItemType Directory -Force -Path $taskPackage | Out-Null
cmake --install $Build --prefix $taskPackage
if($LASTEXITCODE){throw 'CMake installation failed'}
& "$Qt/bin/windeployqt.exe" --release --no-translations --no-system-d3d-compiler --no-opengl-sw $taskPackage/projection-studio.exe
if($LASTEXITCODE){throw 'Qt deployment failed'}
if(Test-Path "$OpenCV/x64/mingw/bin"){Copy-Item "$OpenCV/x64/mingw/bin/*.dll" -Destination $taskPackage}
elseif(Test-Path "$OpenCV/x64/vc17/bin"){Copy-Item "$OpenCV/x64/vc17/bin/*.dll" -Destination $taskPackage}
Copy-Item -LiteralPath docs -Destination $taskPackage -Recurse -Force
Copy-Item -LiteralPath scripts/install.ps1,scripts/uninstall.ps1 -Destination $taskPackage -Force
Copy-Item -LiteralPath licenses -Destination $taskPackage -Recurse -Force
if(Test-Path "$Qt/sbom"){Copy-Item -LiteralPath "$Qt/sbom" -Destination "$taskPackage/licenses/qt-sbom" -Recurse -Force}
Compress-Archive -Path "$taskPackage/*" -DestinationPath 'dist/ProjectionStudio-0.2.0-windows-x64.zip' -Force
if($SourceBundle){
    $taskSources=Join-Path $taskRoot 'dist/ProjectionStudio-0.2.0-source'
    New-Item -ItemType Directory -Force -Path $taskSources | Out-Null
    foreach($taskItem in @('src','include','tests','scripts','examples','docs','licenses','.github','.clang-format','.gitignore','CMakeLists.txt','CMakePresets.json','README.md','LICENSE','COPYRIGHT','THIRD_PARTY_NOTICES.md')){Copy-Item -LiteralPath $taskItem -Destination $taskSources -Recurse -Force}
    $taskDeps=Join-Path $taskSources 'dependency-sources';New-Item -ItemType Directory -Force -Path $taskDeps | Out-Null
    if(Test-Path .tools/source-archives){Copy-Item .tools/source-archives/* -Destination $taskDeps -Force}
    if(Test-Path .tools/opencv-4.11.0.zip){Copy-Item -LiteralPath .tools/opencv-4.11.0.zip -Destination $taskDeps}
    $taskJson=Get-ChildItem -Path "$Build/_deps/json-subbuild/json-populate-prefix/src/json.tar.xz",'build/core/_deps/json-subbuild/json-populate-prefix/src/json.tar.xz' -ErrorAction SilentlyContinue | Select-Object -First 1
    if($taskJson){Copy-Item -LiteralPath $taskJson.FullName -Destination $taskDeps}
    Copy-Item -LiteralPath "$Build/_deps/minhook-subbuild/minhook-populate-prefix/src/v1.3.4.tar.gz" -Destination "$taskDeps/minhook-v1.3.4.tar.gz"
    $taskHashes=@(Get-ChildItem -LiteralPath $taskDeps -File | Where-Object Name -ne 'SHA256SUMS.txt' | Get-FileHash -Algorithm SHA256 | ForEach-Object { "$($_.Hash.ToLower())  $([IO.Path]::GetFileName($_.Path))" })
    $taskHashes | Set-Content -LiteralPath "$taskDeps/SHA256SUMS.txt"
    Compress-Archive -Path "$taskSources/*" -DestinationPath 'dist/ProjectionStudio-0.2.0-source.zip' -Force
}
Write-Host "Portable package: $taskPackage"
