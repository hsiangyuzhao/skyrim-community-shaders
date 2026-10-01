<#!
Build a self-contained batch33 test archive from the current source tree and an
already-built Release DLL. This script deliberately does not invoke Git, CMake,
Visual Studio, or the game.
#>
param(
    [string]$RepositoryRoot = (Join-Path $PSScriptRoot '..'),
    [string]$OutputDirectory = '',
    [string]$PluginDll = '',
    [string]$NrRuntimePath = '',
    [string]$ExpectedNrVersion = '',
    [string]$ExpectedNrSha256 = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repository = (Resolve-Path -LiteralPath $RepositoryRoot).Path
if (-not (Test-Path -LiteralPath (Join-Path $repository 'CMakeLists.txt') -PathType Leaf) -or
    -not (Test-Path -LiteralPath (Join-Path $repository 'features') -PathType Container)) {
    throw "Not a Community Shaders source tree: $repository"
}
if (-not $PluginDll) { $PluginDll = Join-Path $repository 'build/ALL/Release/CommunityShaders.dll' }
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $repository 'dist' }
if (-not (Test-Path -LiteralPath $PluginDll -PathType Leaf)) {
    throw "Release DLL is missing: $PluginDll. Build the ALL Release target first."
}
$nrRuntime = $null
$nrRuntimeVersion = $null
$nrRuntimeHash = $null
if ($NrRuntimePath) {
    if (-not $ExpectedNrVersion -or $ExpectedNrSha256 -notmatch '^[a-fA-F0-9]{64}$') {
        throw 'Explicit NR runtime packaging requires -ExpectedNrVersion and a 64-digit -ExpectedNrSha256.'
    }
    $nrRuntime = Get-Item -LiteralPath $NrRuntimePath -ErrorAction Stop
    if ($nrRuntime.Name -ine 'nvngx_dlssnr.dll') {
        throw 'The explicit NR runtime input must be named nvngx_dlssnr.dll.'
    }
    $nrRuntimeVersion = $nrRuntime.VersionInfo.FileVersionRaw.ToString()
    $nrRuntimeHash = (Get-FileHash -LiteralPath $nrRuntime.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($nrRuntimeVersion -ne $ExpectedNrVersion -or $nrRuntimeHash -ine $ExpectedNrSha256) {
        throw "NR runtime version or SHA-256 mismatch: version=$nrRuntimeVersion SHA-256=$nrRuntimeHash"
    }
}

# The assembled shaders are read from source below, so only compiled inputs
# need to be older than the DLL. This catches a common stale-binary package.
$compiledInputs = @(
    Get-ChildItem -LiteralPath (Join-Path $repository 'src'), (Join-Path $repository 'include') -Recurse -File |
        Where-Object { $_.Extension -in @('.cpp', '.c', '.cc', '.h', '.hpp', '.inl') }
)
$compiledInputs += Get-Item -LiteralPath (Join-Path $repository 'CMakeLists.txt')
$latestInput = $compiledInputs | Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
$builtDll = Get-Item -LiteralPath $PluginDll
if ($builtDll.LastWriteTimeUtc -lt $latestInput.LastWriteTimeUtc) {
    throw "Release DLL is older than $($latestInput.FullName). Rebuild before packaging."
}

New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$stamp = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH-mm-ssZ')
$nonce = [guid]::NewGuid().ToString('N').Substring(0, 8)
$stage = Join-Path $repository "build/ALL/batch33-stage-$stamp-$nonce"
$archive = Join-Path $OutputDirectory "CommunityShaders-batch33-AIO-$stamp-$nonce.zip"
New-Item -ItemType Directory -Path $stage -Force | Out-Null

$excludedNames = @(
    'nvngx_dlssnr.dll', 'CORE', 'SettingsUser.json', 'SettingsDefault.json',
    'SettingsTest.json', 'SettingsTheme.json', 'AppliedOverrides.json',
    'CommunityShaders_ImGui.ini', 'CommunityShaders.log'
)
$excludedBuildExtensions = @('.zip', '.7z', '.pdb', '.ilk', '.obj', '.lib')
function Copy-SourceTree([string]$sourceRoot) {
    $root = (Resolve-Path -LiteralPath $sourceRoot).Path.TrimEnd('\', '/')
    foreach ($file in Get-ChildItem -LiteralPath $root -Recurse -File | Sort-Object FullName) {
        $relative = $file.FullName.Substring($root.Length).TrimStart('\', '/')
        if ($excludedNames -contains $file.Name -or
            $file.Name -ieq 'CommunityShaders.dll' -or
            $file.Name -match '(?i)dlssnr.*\.dll$' -or
            $excludedBuildExtensions -contains $file.Extension -or
            $relative -match '(?i)(^|[\\/])(build|dist|stage|batch33-stage-[^\\/]+)([\\/]|$)') { continue }
        $destination = Join-Path $stage $relative
        $parent = Split-Path -Parent $destination
        New-Item -ItemType Directory -Path $parent -Force | Out-Null
        Copy-Item -LiteralPath $file.FullName -Destination $destination -Force
    }
}

# This is the same overlay layout as CMake install(): package/ first, then
# feature directories in order, with each feature's contents at the Data root.
Copy-SourceTree (Join-Path $repository 'package')
foreach ($feature in Get-ChildItem -LiteralPath (Join-Path $repository 'features') -Directory | Sort-Object Name) {
    Copy-SourceTree $feature.FullName
}
$plugins = Join-Path $stage 'SKSE/Plugins'
New-Item -ItemType Directory -Path $plugins -Force | Out-Null
Copy-Item -LiteralPath $builtDll.FullName -Destination (Join-Path $plugins 'CommunityShaders.dll') -Force
$nrRuntimeDestination = 'Shaders/Upscaling/Streamline/nvngx_dlssnr.dll'
if ($nrRuntime) {
    $target = Join-Path $stage $nrRuntimeDestination
    New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null
    Copy-Item -LiteralPath $nrRuntime.FullName -Destination $target -Force
    if ((Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash.ToLowerInvariant() -ne $nrRuntimeHash) {
        throw 'The staged NR runtime differs from the validated input.'
    }
}
$nrdLicense = Join-Path $repository 'extern/NRD/LICENSE.txt'
if (Test-Path -LiteralPath $nrdLicense -PathType Leaf) {
    $licenseDestination = Join-Path $stage 'Shaders/NRD/nrd.license.txt'
    New-Item -ItemType Directory -Path (Split-Path -Parent $licenseDestination) -Force | Out-Null
    Copy-Item -LiteralPath $nrdLicense -Destination $licenseDestination -Force
}

$testDirectory = Join-Path $stage 'Batch33-Test'
New-Item -ItemType Directory -Path $testDirectory -Force | Out-Null
foreach ($testFile in @('Batch33-Test-README.md', 'Batch33-Test-Matrix.csv',
        'Collect-Batch33Diagnostics.ps1', 'Prepare-Batch33OneLaunch.ps1')) {
    $source = Join-Path $repository "tools/$testFile"
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { throw "Missing test-kit file: $source" }
    $destinationName = $testFile.Replace('Batch33-Test-', '')
    Copy-Item -LiteralPath $source -Destination (Join-Path $testDirectory $destinationName) -Force
}

$stagedFiles = @(Get-ChildItem -LiteralPath $stage -Recurse -File)
$stagedNrRuntimes = @($stagedFiles | Where-Object { $_.Name -match '(?i)dlssnr.*\.dll$' })
if (($nrRuntime -and ($stagedNrRuntimes.Count -ne 1 -or
            $stagedNrRuntimes[0].FullName -ne (Join-Path $stage $nrRuntimeDestination))) -or
    (-not $nrRuntime -and $stagedNrRuntimes.Count -ne 0)) {
    throw 'The staging directory has an unexpected DLSS NR runtime DLL.'
}
if (-not (Test-Path -LiteralPath (Join-Path $stage 'Shaders/Upscaling/Streamline/sl.dlss_g.dll'))) {
    throw 'The staged package is missing the Streamline DLSS-G plugin.'
}

$entries = foreach ($file in $stagedFiles | Sort-Object FullName) {
    $relative = $file.FullName.Substring($stage.Length).TrimStart('\', '/').Replace('\', '/')
    $item = [ordered]@{
        path = $relative
        bytes = $file.Length
        sha256 = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    }
    if ($file.Extension -ieq '.dll') { $item.fileVersion = $file.VersionInfo.FileVersion }
    [pscustomobject]$item
}
$manifest = [ordered]@{
    schema = 1
    label = 'batch33 AIO test'
    builtUtc = $stamp
    preset = 'ALL Release'
    pluginSha256 = (Get-FileHash -LiteralPath $builtDll.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    latestCompiledInput = $latestInput.FullName.Substring($repository.Length).TrimStart('\', '/').Replace('\', '/')
    latestCompiledInputUtc = $latestInput.LastWriteTimeUtc.ToString('o')
    proprietaryNrRuntimeIncluded = [bool]$nrRuntime
    nrRuntimeVersion = $nrRuntimeVersion
    nrRuntimeSha256 = $nrRuntimeHash
    files = @($entries)
}
$manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $testDirectory 'PackageManifest.json') -Encoding UTF8

Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::CreateFromDirectory($stage, $archive, [System.IO.Compression.CompressionLevel]::Optimal, $false)
$zip = [System.IO.Compression.ZipFile]::OpenRead($archive)
try {
    $nrEntries = @($zip.Entries | Where-Object { $_.Name -match '(?i)dlssnr.*\.dll$' })
    if (($nrRuntime -and ($nrEntries.Count -ne 1 -or
                $nrEntries[0].FullName -ne $nrRuntimeDestination)) -or
        (-not $nrRuntime -and $nrEntries.Count -ne 0)) {
        throw 'The completed archive has an unexpected DLSS NR runtime DLL.'
    }
    if ($zip.Entries | Where-Object { $_.FullName -match '(?i)(^|/)(build|dist|stage|batch33-stage-[^/]+)(/|$)' -or
            $_.Name -match '(?i)\.(zip|7z|pdb|ilk|obj|lib)$' -or $_.Name -ieq 'SettingsUser.json' }) {
        throw 'The completed archive contains a staging directory, old build output, or user settings.'
    }
    $pluginEntries = @($zip.Entries | Where-Object { $_.Name -ieq 'CommunityShaders.dll' })
    if ($pluginEntries.Count -ne 1 -or $pluginEntries[0].FullName -ne 'SKSE/Plugins/CommunityShaders.dll') {
        throw 'The completed archive must contain exactly one CommunityShaders.dll at the expected path.'
    }
} finally { $zip.Dispose() }
Write-Output "Package: $archive"
Write-Output "Files: $($entries.Count) plus manifest"
Write-Output "DLL SHA-256: $($manifest.pluginSha256)"
