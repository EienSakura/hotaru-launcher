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
if ([IO.Path]::GetExtension($outPath) -ne '.zip') {
    throw '请指定 ZIP 文件作为输出。'
}
$outputParent = (Resolve-Path -LiteralPath ([IO.Path]::GetDirectoryName($outPath))).ProviderPath
$outPath = Join-Path $outputParent ([IO.Path]::GetFileName($outPath))
$runPath = Join-Path $outputParent ([IO.Path]::GetFileNameWithoutExtension($outPath))
if ((Test-Path -LiteralPath $outPath) -or (Test-Path -LiteralPath $runPath)) {
    throw 'ZIP 或同名运行目录已存在，请指定新的输出名称。'
}
foreach ($required in @('HotaruDev.exe', 'ltoolcore.dll', 'Qt6Core.dll', 'Qt6Svg.dll', 'platforms/qwindows.dll')) {
    if (!(Test-Path -LiteralPath (Join-Path $runtime $required) -PathType Leaf)) {
        throw "缺少运行文件：$required"
    }
}
if (Test-Path -LiteralPath (Join-Path $runtime 'Hotaru.exe')) {
    throw '构建目录混入了正式版，请使用独立的开发版构建目录。'
}

Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$tempDir = Join-Path $outputParent ('.hotaru-package-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $tempDir | Out-Null
$tempZip = Join-Path $tempDir 'package.zip'
$tempExtract = Join-Path $tempDir 'extracted'
try {
    $zip = [IO.Compression.ZipFile]::Open($tempZip, [IO.Compression.ZipArchiveMode]::Create)
    try {
        function Add-DevEntry([string]$source, [string]$name) {
            [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $source, "HotaruDev/$name") | Out-Null
        }
        Add-DevEntry (Join-Path $runtime 'HotaruDev.exe') 'HotaruDev.exe'
        foreach ($dll in Get-ChildItem -LiteralPath $runtime -File -Filter '*.dll') {
            # 旧构建目录里可能还留有 Windows 提供的 ICU 副本。
            if ($dll.Name -ieq 'icuuc.dll') { continue }
            Add-DevEntry $dll.FullName $dll.Name
        }
        # 仅打包运行库；不递归收集配置、缓存、日志或工具插件。
        foreach ($dir in @('platforms', 'imageformats', 'iconengines', 'styles', 'networkinformation', 'tls')) {
            $sourceDir = Join-Path $runtime $dir
            if (!(Test-Path -LiteralPath $sourceDir -PathType Container)) { continue }
            foreach ($dll in Get-ChildItem -LiteralPath $sourceDir -File -Filter '*.dll') {
                if ($dll.Name -notin @('qoffscreen.dll', 'qminimal.dll')) {
                    Add-DevEntry $dll.FullName "$dir/$($dll.Name)"
                }
            }
        }
        foreach ($license in Get-ChildItem -LiteralPath (Join-Path $runtime 'licenses') -File -Filter '*.txt') {
            if ($license.Name -ieq 'BoringSSL-LICENSE.txt') { continue }
            Add-DevEntry $license.FullName "licenses/$($license.Name)"
        }
        Add-DevEntry (Join-Path $project 'LICENSE') 'LICENSE'
        Add-DevEntry (Join-Path $project 'docs/development-testing.md') 'TESTING.md'
        if ('HOTARU_LAN_PREVIEW:BOOL=ON' -in $cache) {
            $lanBuild = Join-Path $runtime 'native-package-build'
            foreach ($dll in @('HotaruLan.dll', 'lan-clipboard.dll', 'lan-files.dll')) {
                Add-DevEntry (Join-Path $lanBuild $dll) "native-plugins-dev/$dll"
            }
            Add-DevEntry (Join-Path $lanBuild 'BoringSSL-LICENSE.txt') 'licenses/BoringSSL-LICENSE.txt'
            Add-DevEntry (Join-Path $project 'docs/lan-preview-testing.md') 'LAN-TESTING.md'
        }
    } finally {
        $zip.Dispose()
    }
    # 从交付的 ZIP 解压，确保两份产物使用同一份文件。
    [IO.Compression.ZipFile]::ExtractToDirectory($tempZip, $tempExtract)
    $tempRun = Join-Path $tempExtract 'HotaruDev'
    if ((Resolve-Path -LiteralPath $tempRun).ProviderPath -ne $tempRun -or
        ((Get-Item -LiteralPath $tempRun).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw '暂存运行目录不在预期位置。'
    }
    [IO.Directory]::Move($tempRun, $runPath)
    [IO.File]::Move($tempZip, $outPath)
} finally {
    if ((Test-Path -LiteralPath $tempDir -PathType Container) -and
        (Resolve-Path -LiteralPath $tempDir).ProviderPath -eq $tempDir -and
        -not ((Get-Item -LiteralPath $tempDir).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        Remove-Item -LiteralPath $tempDir -Recurse -Force
    }
}
$hash = Get-FileHash -LiteralPath $outPath -Algorithm SHA256
[pscustomobject]@{
    Path = $hash.Path
    Hash = $hash.Hash
    RunDirectory = $runPath
    Executable = Join-Path $runPath 'HotaruDev.exe'
}
