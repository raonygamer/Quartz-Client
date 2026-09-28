param(
    [string]$MsysRoot = 'C:\msys64',
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo')][string]$Configuration = 'Release',
    [switch]$Test
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$bash = Join-Path $MsysRoot 'usr\bin\bash.exe'
if (!(Test-Path -LiteralPath $bash)) { throw 'Install MSYS2 and the UCRT64 packages listed in docs/WINDOWS.md first.' }
$env:MSYSTEM = 'UCRT64'
$env:CHERE_INVOKING = '1'
Push-Location $projectRoot
try {
    & $bash -lc "cmake -S . -B build/windows -G Ninja -DCMAKE_BUILD_TYPE=$Configuration -DQUARTZ_BUILD_TESTS=ON"
    if ($LASTEXITCODE) { throw 'CMake configuration failed.' }
    & $bash -lc 'cmake --build build/windows --parallel 4'
    if ($LASTEXITCODE) { throw 'Windows build failed.' }
    if ($Test) {
        & $bash -lc 'ctest --test-dir build/windows --output-on-failure'
        if ($LASTEXITCODE) { throw 'Windows integration tests failed.' }
    }
    & $bash -lc 'cmake --install build/windows --prefix build/Quartz-Windows-x64'
    if ($LASTEXITCODE) { throw 'Portable package creation failed.' }
    Compress-Archive -Path (Join-Path $projectRoot 'build\Quartz-Windows-x64') -DestinationPath (Join-Path $projectRoot 'build\Quartz-Windows-x64.zip') -Force
} finally { Pop-Location }
