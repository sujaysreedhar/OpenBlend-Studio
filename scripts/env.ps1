# Workspace-local toolchain; does not change machine or user PATH.
$taskRoot = Split-Path $PSScriptRoot -Parent
$taskBins = @("$taskRoot/.tools/python/cmake/data/bin", "$taskRoot/.tools/python/bin", "$taskRoot/.tools/Qt/Tools/mingw1310_64/bin", "$taskRoot/.tools/Qt/6.8.3/mingw_64/bin")
$env:PYTHONPATH = "$taskRoot/.tools/python"
$env:PATH = ($taskBins -join ';') + ';' + $env:PATH
$env:CMAKE_PREFIX_PATH = "$taskRoot/.tools/Qt/6.8.3/mingw_64"
foreach($taskCv in @("$taskRoot/.tools/opencv", "$taskRoot/.tools/opencv-mingw")) {
    if(Test-Path $taskCv){
        $env:PATH = "$taskCv/x64/mingw/bin;" + $env:PATH
        $env:OpenCV_DIR = "$taskCv/x64/mingw/lib"
    }
}
