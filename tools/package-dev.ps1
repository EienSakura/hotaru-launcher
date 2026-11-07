param(
    [Parameter(Mandatory)][string]$BuildDirectory,
    [Parameter(Mandatory)][string]$OutputFile
)

$ErrorActionPreference = 'Stop'
$project = Split-Path -Parent $PSScriptRoot
$buildPath = (Resolve-Path -LiteralPath $BuildDirectory).Path
$cache = Get-Content -LiteralPath (Join-Path $buildPath 'CMakeCache.txt')
if ('HOTARU_DEV_BUILD:BOOL=ON' -notin $cache -or 'HOTARU_BUNDLE_PLUGINS:BOOL=OFF' -notin $cache) {
    throw '请使用独立的 HOTARU_DEV_BUILD=ON、HOTARU_BUNDLE_PLUGINS=OFF 构建目录。'
}
$runtime = Join-Path $buildPath 'Release'
$outPath = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputFile)
if ([IO.Path]::GetExtension($outPath) -ne '.zip' -or (Test-Path -LiteralPath $outPath)) {
    throw '请指定尚不存在的 ZIP 文件作为输出。'
}
foreach ($required in @('HotaruDev.exe', 'ltoolcore.dll', 'Qt6Core.dll', 'platforms/qwindows.dll')) {
    if (!(Test-Path -LiteralPath (Join-Path $runtime $required) -PathType Leaf)) {
        throw "缺少运行文件：$required"
    }
}
if (Test-Path -LiteralPath (Join-Path $runtime 'Hotaru.exe')) {
    throw '构建目录混入了正式版，请使用独立的开发版构建目录。'
}

Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$tempZip = $outPath + '.' + [guid]::NewGuid().ToString('N') + '.part'
try {
    $zip = [IO.Compression.ZipFile]::Open($tempZip, [IO.Compression.ZipArchiveMode]::Create)
    try {
        function Add-DevEntry([string]$source, [string]$name) {
            [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $source, "HotaruDev/$name") | Out-Null
        }
        Add-DevEntry (Join-Path $runtime 'HotaruDev.exe') 'HotaruDev.exe'
        foreach ($dll in Get-ChildItem -LiteralPath $runtime -File -Filter '*.dll') {
            Add-DevEntry $dll.FullName $dll.Name
        }
        # 仅打包运行库；不递归收集配置、缓存、日志或工具插件。
        foreach ($dir in @('platforms', 'imageformats', 'styles', 'networkinformation', 'tls')) {
            $sourceDir = Join-Path $runtime $dir
            if (!(Test-Path -LiteralPath $sourceDir -PathType Container)) { continue }
            foreach ($dll in Get-ChildItem -LiteralPath $sourceDir -File -Filter '*.dll') {
                if ($dll.Name -notin @('qoffscreen.dll', 'qminimal.dll')) {
                    Add-DevEntry $dll.FullName "$dir/$($dll.Name)"
                }
            }
        }
        foreach ($license in Get-ChildItem -LiteralPath (Join-Path $runtime 'licenses') -File -Filter '*.txt') {
            Add-DevEntry $license.FullName "licenses/$($license.Name)"
        }
        Add-DevEntry (Join-Path $project 'LICENSE') 'LICENSE'
        Add-DevEntry (Join-Path $project 'docs/development-testing.md') 'TESTING.md'
    } finally {
        $zip.Dispose()
    }
    [IO.File]::Move($tempZip, $outPath)
} finally {
    if (Test-Path -LiteralPath $tempZip) { Remove-Item -LiteralPath $tempZip }
}
Get-FileHash -LiteralPath $outPath -Algorithm SHA256
