param(
    [Parameter(Mandatory)][ValidatePattern('^[a-z0-9]+(?:-[a-z0-9]+)*$')][string]$PluginId,
    [Parameter(Mandatory)][ValidatePattern('^[0-9]+\.[0-9]+\.[0-9]+(?:-[a-zA-Z0-9.-]+)?$')][string]$Version,
    [Parameter(Mandatory)][string]$Dll,
    [Parameter(Mandatory)][string]$OutputDirectory,
    [string[]]$License = @()
)

$ErrorActionPreference = 'Stop'
$dllFile = Get-Item -LiteralPath $Dll
if ($dllFile -isnot [IO.FileInfo] -or $dllFile.Extension -ine '.dll') {
    throw '每个插件包需要一个 DLL。'
}
$parent = Get-Item -LiteralPath $OutputDirectory
if ($parent -isnot [IO.DirectoryInfo]) { throw '输出目录不存在。' }
$name = "hotaru-plugin-$PluginId-$Version-x64.zip"
if ($name -cnotmatch '^hotaru-plugin-[a-z0-9-]+-[0-9]+\.[0-9]+\.[0-9]+(?:-[A-Za-z0-9.-]+)?-x64\.zip$') {
    throw '插件包名必须以小写 hotaru-plugin- 开头。'
}
$destination = Join-Path $parent.FullName $name
if (Test-Path -LiteralPath $destination) { throw "输出已存在：$destination" }
$files = @($dllFile)
foreach ($path in $License) {
    $file = Get-Item -LiteralPath $path
    if ($file -isnot [IO.FileInfo] -or $file.Extension -ine '.txt') { throw '许可附件必须是文本文件。' }
    $files += $file
}
if (($files.Name | Sort-Object -Unique).Count -ne $files.Count) { throw '包内文件名重复。' }

Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$temporary = Join-Path $parent.FullName ('.plugin-' + [guid]::NewGuid().ToString('N') + '.zip')
try {
    $zip = [IO.Compression.ZipFile]::Open($temporary, [IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($file in $files) {
            [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $file.FullName, $file.Name) | Out-Null
        }
    } finally { $zip.Dispose() }
    [IO.File]::Move($temporary, $destination)
} finally {
    if (Test-Path -LiteralPath $temporary -PathType Leaf) { Remove-Item -LiteralPath $temporary }
}
Get-FileHash -LiteralPath $destination -Algorithm SHA256
