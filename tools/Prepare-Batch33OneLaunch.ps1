#Requires -Version 7.0
param(
    [Parameter(Mandatory = $true)][string]$DataPath,
    [string]$SettingsDirectory = '',
    [switch]$Restore
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$data = (Resolve-Path -LiteralPath $DataPath).Path
if (-not $SettingsDirectory) { $SettingsDirectory = Join-Path $data 'SKSE/Plugins/CommunityShaders' }
New-Item -ItemType Directory -Path $SettingsDirectory -Force | Out-Null
$settingsPath = Join-Path $SettingsDirectory 'SettingsUser.json'
$reportPath = Join-Path $SettingsDirectory 'Batch33-Preflight.json'

function Read-JsonObject([string]$path) {
    $value = Get-Content -LiteralPath $path -Raw -Encoding UTF8 | ConvertFrom-Json -AsHashtable
    if ($value -isnot [System.Collections.IDictionary]) { throw "Expected a JSON object: $path" }
    return $value
}
function Save-JsonObject([string]$path, [System.Collections.IDictionary]$value) {
    $temporary = "$path.batch33-writing-$([guid]::NewGuid().ToString('N'))"
    $value | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $temporary -Encoding UTF8
    Move-Item -LiteralPath $temporary -Destination $path -Force
}

if ($Restore) {
    if (-not (Test-Path -LiteralPath $reportPath -PathType Leaf)) { throw "No batch33 preflight report: $reportPath" }
    if (-not (Test-Path -LiteralPath $settingsPath -PathType Leaf)) {
        throw "SettingsUser.json is missing. The original backup named in $reportPath remains available for manual recovery."
    }
    $report = Read-JsonObject $reportPath
    if ($report['restoredUtc']) { throw "Preflight was already restored at $($report['restoredUtc'])." }
    $settings = Read-JsonObject $settingsPath
    if (-not $settings.Contains('Upscaling')) { $settings['Upscaling'] = [ordered]@{} }
    if ($settings['Upscaling'] -isnot [System.Collections.IDictionary]) {
        throw 'Upscaling settings are no longer a JSON object; refusing to overwrite them.'
    }
    $upscaling = $settings['Upscaling']
    foreach ($entry in @(
            @('frameGenerationBackend', 'originalBackendExisted', 'originalBackend'),
            @('upscaleMethod', 'originalUpscaleMethodExisted', 'originalUpscaleMethod'))) {
        if ($report[$entry[1]]) { $upscaling[$entry[0]] = $report[$entry[2]] }
        else { $upscaling.Remove($entry[0]) }
    }
    if (-not $report['originalUpscalingExisted'] -and $upscaling.Count -eq 0) {
        $settings.Remove('Upscaling')
    }
    if (-not $report['originalFileExisted'] -and $settings.Count -eq 0) {
        Remove-Item -LiteralPath $settingsPath
    } else {
        Save-JsonObject $settingsPath $settings
    }
    $report['restoredUtc'] = (Get-Date).ToUniversalTime().ToString('o')
    Save-JsonObject $reportPath $report
    Write-Output "Restored the two batch33 preflight keys in: $settingsPath"
    if ($report['originalFileExisted']) {
        Write-Output "Original byte-for-byte backup remains: $(Join-Path $SettingsDirectory $report['backupFile'])"
    }
    return
}

if (Test-Path -LiteralPath $reportPath -PathType Leaf) {
    throw "A preflight report already exists: $reportPath. Restore it before preparing again."
}
$originalExisted = Test-Path -LiteralPath $settingsPath -PathType Leaf
$stamp = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH-mm-ssZ')
$backupName = if ($originalExisted) { "SettingsUser.batch33-backup-$stamp-$([guid]::NewGuid().ToString('N').Substring(0, 8)).json" } else { '' }
if ($originalExisted) {
    Copy-Item -LiteralPath $settingsPath -Destination (Join-Path $SettingsDirectory $backupName)
    $settings = Read-JsonObject $settingsPath
} else {
    $settings = [ordered]@{}
}
$upscalingExisted = $settings.Contains('Upscaling')
if (-not $upscalingExisted) { $settings['Upscaling'] = [ordered]@{} }
if ($settings['Upscaling'] -isnot [System.Collections.IDictionary]) {
    throw 'Upscaling settings are not a JSON object; refusing to overwrite them.'
}
$upscaling = $settings['Upscaling']
$backendExisted = $upscaling.Contains('frameGenerationBackend')
$methodExisted = $upscaling.Contains('upscaleMethod')
$oldBackend = if ($backendExisted) { $upscaling['frameGenerationBackend'] } else { $null }
$oldMethod = if ($methodExisted) { $upscaling['upscaleMethod'] } else { $null }
$upscaling['frameGenerationBackend'] = 1
$upscaling['upscaleMethod'] = 3
Save-JsonObject $settingsPath $settings

$report = [ordered]@{
    schema = 1
    preparedUtc = (Get-Date).ToUniversalTime().ToString('o')
    originalFileExisted = [bool]$originalExisted
    backupFile = $backupName
    originalUpscalingExisted = [bool]$upscalingExisted
    originalBackendExisted = [bool]$backendExisted
    originalBackend = $oldBackend
    originalUpscaleMethodExisted = [bool]$methodExisted
    originalUpscaleMethod = $oldMethod
    preparedBackend = 1
    preparedUpscaleMethod = 3
    restoredUtc = $null
}
Save-JsonObject $reportPath $report
Write-Output "Prepared one-launch DLSS-G test settings: $settingsPath"
Write-Output 'Only Upscaling.frameGenerationBackend=1 and Upscaling.upscaleMethod=3 were changed.'
if ($originalExisted) { Write-Output "Original byte-for-byte backup: $(Join-Path $SettingsDirectory $backupName)" }
Write-Output "Preparation report: $reportPath"
