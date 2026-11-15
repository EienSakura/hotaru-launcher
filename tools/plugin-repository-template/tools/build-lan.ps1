param(
    [Parameter(Mandatory)][string]$VcpkgRoot,
    [Parameter(Mandatory)][string]$BoringSslSource,
    [Parameter(Mandatory)][string]$BoringSslCrypto,
    [string]$InstalledDirectory,
    [string]$BuildDirectory = 'build/lan',
    [string]$OutputDirectory = 'dist'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$build = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($BuildDirectory)
$out = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputDirectory)
$crypto = (Resolve-Path -LiteralPath $BoringSslCrypto).Path
$boring = (Resolve-Path -LiteralPath $BoringSslSource).Path
$extra = @()
$installed = Join-Path $build 'vcpkg_installed'
if ($InstalledDirectory) {
    $installed = (Resolve-Path -LiteralPath $InstalledDirectory).Path
    $extra = @("-DVCPKG_INSTALLED_DIR=$installed", '-DVCPKG_MANIFEST_INSTALL=OFF')
}
& cmake -S $root -B $build -G 'Visual Studio 17 2022' -A x64 "-DCMAKE_TOOLCHAIN_FILE=$VcpkgRoot/scripts/buildsystems/vcpkg.cmake" "-DHOTARU_BORINGSSL_SOURCE=$boring" "-DHOTARU_BORINGSSL_CRYPTO=$crypto" -DVCPKG_TARGET_TRIPLET=x64-windows @extra
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& cmake --build $build --config Release --target hotaru-lan-module hotaru-lan-clipboard hotaru-lan-files hotaru-lan-selftest --parallel 4
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$qt = Join-Path $installed 'x64-windows'
$env:PATH = "$(Join-Path $qt 'bin');$env:PATH"
$env:QT_QPA_PLATFORM = 'offscreen'
$env:QT_PLUGIN_PATH = Join-Path $qt 'Qt6/plugins'
if (!(Test-Path -LiteralPath (Join-Path $env:QT_PLUGIN_PATH 'platforms/qoffscreen.dll'))) { throw '缺少 Qt 离屏后端' }
& (Join-Path $build 'Release/hotaru-lan-selftest.exe')
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
New-Item -ItemType Directory -Force -Path $out | Out-Null
$modules = Join-Path $build 'Release/native-package-build'
foreach ($spec in @(@('lan-core','HotaruLan','plugin'),@('lan-clipboard','lan-clipboard','clipboard'),@('lan-files','lan-files','files'))) {
    $meta = Get-Content -Raw -Encoding utf8 -LiteralPath (Join-Path $build "plugins/lan-core/published-$($spec[2]).json") | ConvertFrom-Json
    $params = @{PluginId=$spec[0]; Version=$meta.Version; Dll=(Join-Path $modules "$($spec[1]).dll"); OutputDirectory=$out}
    if ($spec[0] -eq 'lan-core') { $params.License=@((Join-Path $modules 'BoringSSL-LICENSE.txt')) }
    & (Join-Path $PSScriptRoot 'package-plugin.ps1') @params
    Copy-Item -LiteralPath (Join-Path $build "plugins/lan-core/published-$($spec[2]).json") -Destination (Join-Path $out "$($spec[0]).metadata.json")
}
