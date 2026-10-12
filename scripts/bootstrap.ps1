param([ValidateSet('mingw','msvc')][string]$Compiler='mingw', [switch]$WithOpenCV)
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path $PSScriptRoot -Parent
Set-Location $taskRoot
function Check-Exit { if ($LASTEXITCODE -ne 0) { throw "Command failed: $LASTEXITCODE" } }
python -m pip install --target .tools/python aqtinstall==3.2.1 cmake==3.31.6 ninja==1.11.1.3
Check-Exit
$env:PYTHONPATH = "$taskRoot/.tools/python"
if ($Compiler -eq 'mingw') {
    $taskArch = 'win64_mingw'; $taskQtFolder = 'mingw_64'
    python -m aqt install-tool windows desktop tools_mingw1310 qt.tools.win64_mingw1310 -O .tools/Qt
    Check-Exit
} else { $taskArch = 'win64_msvc2022_64'; $taskQtFolder = 'msvc2022_64' }
python -m aqt install-qt windows desktop 6.8.3 $taskArch -O .tools/Qt --archives qtbase qttools
Check-Exit
python -m aqt install-qt windows desktop 6.8.3 $taskArch -O .tools/Qt --modules qtwebsockets --noarchives
Check-Exit
$taskBins = @("$taskRoot/.tools/python/cmake/data/bin", "$taskRoot/.tools/python/bin", "$taskRoot/.tools/Qt/6.8.3/$taskQtFolder/bin")
if ($Compiler -eq 'mingw') { $taskBins += "$taskRoot/.tools/Qt/Tools/mingw1310_64/bin" }
$env:PATH = ($taskBins -join ';') + ';' + $env:PATH
$env:CMAKE_PREFIX_PATH = "$taskRoot/.tools/Qt/6.8.3/$taskQtFolder"
if ($WithOpenCV) {
    $taskZip = "$taskRoot/.tools/opencv-4.11.0.zip"
    if (!(Test-Path $taskZip)) { Invoke-WebRequest 'https://github.com/opencv/opencv/archive/refs/tags/4.11.0.zip' -OutFile $taskZip }
    if ((Get-FileHash $taskZip -Algorithm SHA256).Hash -ne '11DBD2C8D248FA97AC7D20F33C4BAB8559EF32835D1C9274009150BB2CF5218A') { throw 'OpenCV checksum mismatch' }
    if (!(Test-Path .tools/opencv-source/opencv-4.11.0)) { Expand-Archive -LiteralPath $taskZip -DestinationPath .tools/opencv-source }
    $taskCompilerArgs = if ($Compiler -eq 'mingw') { @('-DCMAKE_C_COMPILER=gcc','-DCMAKE_CXX_COMPILER=g++') } else { @('-DCMAKE_C_COMPILER=cl','-DCMAKE_CXX_COMPILER=cl') }
    cmake -S .tools/opencv-source/opencv-4.11.0 -B ".tools/opencv-build-$Compiler" -G Ninja @taskCompilerArgs -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$taskRoot/.tools/opencv-$Compiler" '-DBUILD_LIST=core,imgproc,calib3d,imgcodecs,videoio' -DBUILD_SHARED_LIBS=ON -DBUILD_TESTS=OFF -DBUILD_PERF_TESTS=OFF -DBUILD_EXAMPLES=OFF -DBUILD_opencv_apps=OFF -DBUILD_JAVA=OFF -DBUILD_opencv_python2=OFF -DBUILD_opencv_python3=OFF -DWITH_IPP=OFF -DWITH_FFMPEG=OFF -DWITH_OPENEXR=OFF -DWITH_PROTOBUF=OFF -DWITH_OPENCL=OFF -DWITH_ITT=OFF -DWITH_WEBP=OFF -DWITH_JPEG=OFF -DWITH_PNG=OFF -DWITH_TIFF=OFF -DWITH_OPENJPEG=OFF -DOPENCV_ENABLE_NONFREE=OFF
    Check-Exit
    cmake --build ".tools/opencv-build-$Compiler" --parallel 4
    Check-Exit
    cmake --install ".tools/opencv-build-$Compiler"
    Check-Exit
    $env:OpenCV_DIR = "$taskRoot/.tools/opencv-$Compiler"
}
Write-Host "Toolchain ready. CMAKE_PREFIX_PATH=$env:CMAKE_PREFIX_PATH"
