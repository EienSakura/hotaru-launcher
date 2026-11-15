param([Parameter(Mandatory)][string]$Destination)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$out = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Destination)
if (Test-Path -LiteralPath $out) { throw '目标已存在；请选择新的空目录。' }
$parent = Split-Path -Parent $out
$stage = Join-Path $parent ('.hotaru-plugins-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $stage | Out-Null
foreach ($item in Get-ChildItem -LiteralPath (Join-Path $PSScriptRoot 'plugin-repository-template') -Force) {
    Copy-Item -LiteralPath $item.FullName -Destination $stage -Recurse
}
New-Item -ItemType Directory -Path (Join-Path $stage 'sdk') | Out-Null
Copy-Item -LiteralPath (Join-Path $root 'src/hotaru-native-plugin.h') -Destination (Join-Path $stage 'sdk')
Copy-Item -LiteralPath (Join-Path $root 'native-plugins/lan') -Destination (Join-Path $stage 'plugins/lan-core') -Recurse
Copy-Item -LiteralPath (Join-Path $root 'LICENSE') -Destination $stage
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'package-plugin.ps1') -Destination (Join-Path $stage 'tools')
Copy-Item -LiteralPath (Join-Path $root 'vcpkg.json') -Destination $stage
foreach ($feature in @('clipboard','files')) {
    $featureRoot = Join-Path $stage "plugins/lan-$feature"
    New-Item -ItemType Directory -Path $featureRoot | Out-Null
    Move-Item -LiteralPath (Join-Path $stage "plugins/lan-core/$feature.json") -Destination $featureRoot
}
[IO.Directory]::Move($stage, $out)
Write-Output $out
