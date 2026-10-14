param(
    [Parameter(Mandatory=$true)][string]$QtRoot,
    [string]$CompilerRoot = '',
    [string]$ServerConfig = '',
    [string]$PythonExecutable = '',
    [string]$CMake = 'cmake',
    [string]$Ninja = 'ninja'
)
$ErrorActionPreference = 'Stop'
$ProjectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$BuildDir = Join-Path $ProjectRoot 'ForWin/build'
if (-not (Test-Path (Join-Path $QtRoot 'bin/Qt6Core.dll'))) { throw 'QtRoot must be a Windows Qt 6.5+ desktop kit.' }
$env:PATH = "$(Join-Path $QtRoot 'bin');$env:PATH"
if ($CompilerRoot) { $env:PATH = "$(Join-Path $CompilerRoot 'bin');$env:PATH" }
# MSVC users run this from Developer PowerShell; MinGW users pass CompilerRoot.
if (-not $ServerConfig) {
    $candidate = Join-Path $ProjectRoot 'config/linkdesk-server.json'
    if (Test-Path $candidate) { $ServerConfig = $candidate }
}
$argsConfigure = @('-S', (Join-Path $ProjectRoot 'ForWin'), '-B', $BuildDir, '-G', 'Ninja',
    "-DCMAKE_PREFIX_PATH=$QtRoot", "-DCMAKE_MAKE_PROGRAM=$Ninja", '-DCMAKE_BUILD_TYPE=Release')
if ($CompilerRoot) {
    $Compiler = Join-Path $CompilerRoot "bin/g++.exe"
    if (-not (Test-Path $Compiler)) { throw "MinGW g++.exe not found." }
    $argsConfigure += "-DCMAKE_CXX_COMPILER=$Compiler"
}
if ($ServerConfig) {
    if (-not (Test-Path $ServerConfig)) { throw 'Server config file not found.' }
    $argsConfigure += "-DLINKDESK_SERVER_CONFIG=$([IO.Path]::GetFullPath($ServerConfig))"
}
if ($PythonExecutable) { $argsConfigure += "-DPython3_EXECUTABLE=$PythonExecutable" }
& $CMake @argsConfigure
if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed.' }
& $CMake --build $BuildDir --parallel 6
if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }
$Ctest = Join-Path (Split-Path (Get-Command $CMake).Source) 'ctest.exe'
& $Ctest --test-dir $BuildDir --output-on-failure
if ($LASTEXITCODE -ne 0) { throw 'Tests failed.' }
Write-Host "Built: $BuildDir/LinkDesk.exe"
