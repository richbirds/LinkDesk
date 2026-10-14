param(
    [Parameter(Mandatory=$true)][string]$QtRoot,
    [string]$CompilerRoot = '',
    [string]$BuildDir = ''
)
$ErrorActionPreference = 'Stop'
$ProjectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
if (-not $BuildDir) { $BuildDir = Join-Path $ProjectRoot 'ForWin/build' }
$DistRoot = Join-Path $ProjectRoot 'ForWin/dist'
$Destination = Join-Path $DistRoot 'LinkDesk'
if (-not (Test-Path "$BuildDir/LinkDesk.exe")) { throw 'Build LinkDesk first.' }
if (Test-Path $Destination) {
    $Archive = Join-Path $DistRoot "archive/LinkDesk-$(Get-Date -Format 'yyyyMMdd-HHmmss')"
    New-Item -ItemType Directory -Force -Path (Split-Path $Archive) | Out-Null
    Move-Item $Destination $Archive
}
New-Item -ItemType Directory -Force -Path $Destination | Out-Null
Copy-Item "$BuildDir/LinkDesk.exe" $Destination
if (Test-Path "$BuildDir/server.json") { Copy-Item "$BuildDir/server.json" $Destination }
$env:PATH = "$(Join-Path $QtRoot 'bin');$env:PATH"
if ($CompilerRoot) { $env:PATH = "$(Join-Path $CompilerRoot 'bin');$env:PATH" }
& "$QtRoot/bin/windeployqt.exe" --release --compiler-runtime --no-translations --dir $Destination "$Destination/LinkDesk.exe"
if ($LASTEXITCODE -ne 0) { throw 'windeployqt failed.' }
foreach ($Required in @('Qt6Core.dll','Qt6Gui.dll','Qt6Widgets.dll','Qt6Network.dll','platforms/qwindows.dll','tls/qschannelbackend.dll','imageformats/qjpeg.dll')) {
    if (-not (Test-Path (Join-Path $Destination $Required))) { throw "Missing deployed dependency: $Required" }
}
# LinkDesk explicitly uses Windows Schannel; OpenSSL CLI/DLLs are not required.
@('[Paths]', 'Plugins = .') | Set-Content "$Destination/qt.conf" -Encoding ascii
Copy-Item (Join-Path $ProjectRoot 'ForWin/README.md') "$Destination/README.md"
Copy-Item (Join-Path $ProjectRoot 'third_party/qt') (Join-Path $Destination 'licenses/Qt') -Recurse -Force
# Smoke launch only loads UI/network; it never enables capture or input permissions.
$SavedPath = $env:PATH
try {
    $env:PATH = "$env:SystemRoot/System32;$env:SystemRoot"
    $Process = Start-Process "$Destination/LinkDesk.exe" -ArgumentList @('--screenshot', ('"' + "$Destination/startup.png" + '"')) -WorkingDirectory $Destination -WindowStyle Hidden -Wait -PassThru
    if ($Process.ExitCode -ne 0 -or -not (Test-Path "$Destination/startup.png")) { throw 'Packaged client startup failed.' }
} finally { $env:PATH = $SavedPath }
# The smoke screenshot may show a saved host password; keep it out of distributable bundles.
Remove-Item "$Destination/startup.png" -ErrorAction SilentlyContinue
Compress-Archive -Path $Destination -DestinationPath "$DistRoot/LinkDesk-Windows-x64.zip" -Force
Write-Host "Packaged: $DistRoot/LinkDesk-Windows-x64.zip"
