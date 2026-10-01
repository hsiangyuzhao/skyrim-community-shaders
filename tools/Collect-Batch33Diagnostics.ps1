param(
    [string]$DataPath = '',
    [string]$SettingsDirectory = '',
    [string]$LogPath = '',
    [string]$MatrixPath = '',
    [string]$OutputDirectory = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if (-not $DataPath) {
    $candidate = Split-Path -Parent $PSScriptRoot
    if (Test-Path -LiteralPath (Join-Path $candidate 'SKSE/Plugins/CommunityShaders.dll')) {
        $DataPath = $candidate
    } else {
        throw 'Pass -DataPath with the Skyrim Data directory (or the mod-manager Data root).'
    }
}
$data = (Resolve-Path -LiteralPath $DataPath).Path
if (-not $SettingsDirectory) { $SettingsDirectory = Join-Path $data 'SKSE/Plugins/CommunityShaders' }
if (-not $MatrixPath) { $MatrixPath = Join-Path $PSScriptRoot 'Matrix.csv' }
if (-not $OutputDirectory) { $OutputDirectory = Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'Batch33-Diagnostics' }
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null

$stamp = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH-mm-ssZ')
$nonce = [guid]::NewGuid().ToString('N').Substring(0, 8)
$stage = Join-Path $OutputDirectory "batch33-diagnostics-stage-$stamp-$nonce"
$archive = Join-Path $OutputDirectory "Batch33-Diagnostics-$stamp-$nonce.zip"
New-Item -ItemType Directory -Path $stage -Force | Out-Null

function Copy-IfPresent([string]$source, [string]$subdirectory) {
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { return $false }
    $destination = Join-Path $stage $subdirectory
    New-Item -ItemType Directory -Path $destination -Force | Out-Null
    Copy-Item -LiteralPath $source -Destination (Join-Path $destination (Split-Path -Leaf $source)) -Force
    return $true
}

$settingsCopied = @()
foreach ($name in @('SettingsUser.json', 'SettingsDefault.json', 'SettingsTest.json',
        'SettingsTheme.json', 'AppliedOverrides.json', 'Batch33-Preflight.json')) {
    $path = Join-Path $SettingsDirectory $name
    if (Copy-IfPresent $path 'settings') { $settingsCopied += $name }
}

if (-not $LogPath) {
    $documents = [Environment]::GetFolderPath('MyDocuments')
    $candidates = @(
        Join-Path $documents 'My Games/Skyrim Special Edition/SKSE/CommunityShaders.log'
        Join-Path $documents 'My Games/Skyrim VR/SKSE/CommunityShaders.log'
        Join-Path $documents 'My Games/Skyrim/SKSE/CommunityShaders.log'
    )
    $existing = @($candidates | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } |
        ForEach-Object { Get-Item -LiteralPath $_ } | Sort-Object LastWriteTimeUtc -Descending)
    if ($existing.Count -gt 0) { $LogPath = $existing[0].FullName }
}
$logCopied = $false
if ($LogPath) {
    $logCopied = Copy-IfPresent $LogPath 'logs'
    if ($logCopied) {
        $markers = @(Select-String -LiteralPath $LogPath -Pattern '\[B33AB\]' | ForEach-Object { $_.Line })
        $markers | Set-Content -LiteralPath (Join-Path $stage 'logs/B33AB.log') -Encoding UTF8
    }
}
$matrixCopied = Copy-IfPresent $MatrixPath 'tests'
$packageManifest = Join-Path $PSScriptRoot 'PackageManifest.json'
$packageManifestCopied = Copy-IfPresent $packageManifest 'package'

$versionFiles = @()
$pluginDll = Join-Path $data 'SKSE/Plugins/CommunityShaders.dll'
if (Test-Path -LiteralPath $pluginDll -PathType Leaf) { $versionFiles += Get-Item -LiteralPath $pluginDll }
$streamlineDirectory = Join-Path $data 'Shaders/Upscaling/Streamline'
if (Test-Path -LiteralPath $streamlineDirectory -PathType Container) {
    $versionFiles += @(Get-ChildItem -LiteralPath $streamlineDirectory -File -Filter '*.dll')
}
$versions = foreach ($file in $versionFiles | Sort-Object FullName) {
    # For nvngx_dlssnr.dll this records metadata only. The DLL is never copied.
    [pscustomobject][ordered]@{
        name = $file.Name
        bytes = $file.Length
        fileVersion = $file.VersionInfo.FileVersion
        sha256 = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    }
}
$expectedPluginHash = $null
if ($packageManifestCopied) {
    try { $expectedPluginHash = (Get-Content -LiteralPath $packageManifest -Raw -Encoding UTF8 | ConvertFrom-Json).pluginSha256 }
    catch { }
}
$actualPluginHash = if (Test-Path -LiteralPath $pluginDll -PathType Leaf) {
    (Get-FileHash -LiteralPath $pluginDll -Algorithm SHA256).Hash.ToLowerInvariant()
} else { $null }
$pluginMatchesPackage = if ($expectedPluginHash -and $actualPluginHash) {
    $actualPluginHash -ieq $expectedPluginHash
} else { $null }
$gpu = @()
try {
    $gpu = @(Get-CimInstance Win32_VideoController -ErrorAction Stop |
        ForEach-Object { [pscustomobject]@{ name = $_.Name; driverVersion = $_.DriverVersion } })
} catch { }

$report = [ordered]@{
    schema = 1
    collectedUtc = $stamp
    osVersion = [Environment]::OSVersion.VersionString
    gpu = $gpu
    settingsFiles = $settingsCopied
    communityShadersLogIncluded = $logCopied
    testMatrixIncluded = $matrixCopied
    packageManifestIncluded = $packageManifestCopied
    installedPluginMatchesPackage = $pluginMatchesPackage
    runtimeBinariesIncluded = $false
    binaries = @($versions)
}
$report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $stage 'DiagnosticManifest.json') -Encoding UTF8

Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::CreateFromDirectory($stage, $archive, [System.IO.Compression.CompressionLevel]::Optimal, $false)
$zip = [System.IO.Compression.ZipFile]::OpenRead($archive)
try {
    if ($zip.Entries | Where-Object { $_.Name -match '(?i)\.dll$' }) {
        throw 'A runtime DLL entered the diagnostics archive.'
    }
} finally { $zip.Dispose() }
Write-Output "Diagnostics: $archive"
Write-Output "Settings: $($settingsCopied -join ', ')"
Write-Output "CommunityShaders.log: $logCopied; B33AB markers: $(if ($logCopied) { $markers.Count } else { 0 })"
