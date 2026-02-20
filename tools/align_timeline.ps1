param(
    [string]$SessionDir = "",
    [string]$OutputPrefix = "aligned"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Get-LatestSessionDir {
    param([string]$Root)
    if (!(Test-Path $Root)) {
        throw "Capture root not found: $Root"
    }
    $latest = Get-ChildItem -Path $Root -Directory | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($null -eq $latest) {
        throw "No capture sessions found in: $Root"
    }
    return $latest.FullName
}

function Require-File {
    param([string]$Path)
    if (!(Test-Path $Path)) {
        throw "Required file not found: $Path"
    }
}

function As-DoubleOrZero {
    param($Value)
    if ($null -eq $Value) { return 0.0 }
    return [double]$Value
}

function Convert-QpcToUtcNs {
    param(
        [double]$Qpc,
        [double]$AnchorQpc,
        [double]$AnchorUtcNs,
        [double]$QpcFreq
    )
    if ($QpcFreq -le 0.0) { return 0 }
    $utc = $AnchorUtcNs + (($Qpc - $AnchorQpc) * 1000000000.0 / $QpcFreq)
    return [int64][Math]::Round($utc)
}

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
if ([string]::IsNullOrWhiteSpace($SessionDir)) {
    $SessionDir = Get-LatestSessionDir -Root (Join-Path $repoRoot "captures")
} else {
    $SessionDir = (Resolve-Path $SessionDir).Path
}

$metaPath = Join-Path $SessionDir "video_meta.json"
$inputPath = Join-Path $SessionDir "input.jsonl"
Require-File $metaPath
Require-File $inputPath

$meta = Get-Content -Path $metaPath -Raw | ConvertFrom-Json

$qpcFreq = As-DoubleOrZero $meta.qpc_freq
$screenStartQpc = As-DoubleOrZero $meta.screen_start_qpc
$inputStartQpc = As-DoubleOrZero $meta.input_start_qpc
$anchorQpc = As-DoubleOrZero $meta.utc_anchor.qpc_ticks
$anchorUtcNs = As-DoubleOrZero $meta.utc_anchor.utc_epoch_ns

if ($qpcFreq -le 0.0) {
    throw "Invalid qpc_freq in video_meta.json"
}

$alignedJsonlPath = Join-Path $SessionDir ("{0}_input.jsonl" -f $OutputPrefix)
$alignedCsvPath = Join-Path $SessionDir ("{0}_events.csv" -f $OutputPrefix)
$reportPath = Join-Path $SessionDir ("{0}_report.json" -f $OutputPrefix)

$lines = Get-Content -Path $inputPath
$alignedLines = New-Object System.Collections.Generic.List[string]
$csvRows = New-Object System.Collections.Generic.List[object]

$eventCount = 0
$firstVideoMs = $null
$lastVideoMs = $null

foreach ($line in $lines) {
    if ([string]::IsNullOrWhiteSpace($line)) {
        continue
    }

    $obj = $line | ConvertFrom-Json
    $objHash = [ordered]@{}
    foreach ($p in $obj.PSObject.Properties) {
        $objHash[$p.Name] = $p.Value
    }

    if ($objHash.Contains("t_qpc")) {
        $tQpc = As-DoubleOrZero $objHash["t_qpc"]
        $tVideoMs = (($tQpc - $screenStartQpc) * 1000.0 / $qpcFreq)
        $tInputMs = (($tQpc - $inputStartQpc) * 1000.0 / $qpcFreq)
        $tUtcNs = Convert-QpcToUtcNs -Qpc $tQpc -AnchorQpc $anchorQpc -AnchorUtcNs $anchorUtcNs -QpcFreq $qpcFreq

        $objHash["t_video_ms"] = [Math]::Round($tVideoMs, 3)
        $objHash["t_input_ms"] = [Math]::Round($tInputMs, 3)
        $objHash["t_utc_epoch_ns"] = $tUtcNs

        if ($null -eq $firstVideoMs) { $firstVideoMs = $tVideoMs }
        $lastVideoMs = $tVideoMs
        $eventCount++

        $keyName = if ($objHash.Contains("key_name")) { [string]$objHash["key_name"] } else { "" }
        $button = if ($objHash.Contains("button")) { [string]$objHash["button"] } else { "" }
        $csvRows.Add([pscustomobject]@{
            type = [string]$objHash["type"]
            t_qpc = [int64]$tQpc
            t_video_ms = [Math]::Round($tVideoMs, 3)
            t_input_ms = [Math]::Round($tInputMs, 3)
            key_name = $keyName
            button = $button
            wheel_delta = if ($objHash.Contains("wheel_delta")) { $objHash["wheel_delta"] } else { $null }
        }) | Out-Null
    } elseif ([string]$objHash["type"] -eq "session_header") {
        $objHash["timeline_alignment"] = [ordered]@{
            screen_start_qpc = [int64]$screenStartQpc
            input_start_qpc = [int64]$inputStartQpc
            qpc_freq = [int64]$qpcFreq
            input_minus_screen_ms = [Math]::Round((($inputStartQpc - $screenStartQpc) * 1000.0 / $qpcFreq), 3)
            utc_anchor_qpc = [int64]$anchorQpc
            utc_anchor_ns = [int64]$anchorUtcNs
        }
    }

    $alignedLines.Add(($objHash | ConvertTo-Json -Compress -Depth 20)) | Out-Null
}

$alignedLines | Set-Content -Path $alignedJsonlPath -Encoding UTF8
$csvRows | Export-Csv -Path $alignedCsvPath -NoTypeInformation -Encoding UTF8

$report = [ordered]@{
    session_dir = $SessionDir
    source_files = [ordered]@{
        video_meta = $metaPath
        input_jsonl = $inputPath
    }
    output_files = [ordered]@{
        aligned_input_jsonl = $alignedJsonlPath
        aligned_events_csv = $alignedCsvPath
    }
    qpc_freq = [int64]$qpcFreq
    screen_start_qpc = [int64]$screenStartQpc
    input_start_qpc = [int64]$inputStartQpc
    input_minus_screen_ms = [Math]::Round((($inputStartQpc - $screenStartQpc) * 1000.0 / $qpcFreq), 3)
    event_count = $eventCount
    first_event_video_ms = if ($null -ne $firstVideoMs) { [Math]::Round($firstVideoMs, 3) } else { $null }
    last_event_video_ms = if ($null -ne $lastVideoMs) { [Math]::Round($lastVideoMs, 3) } else { $null }
    generated_utc = (Get-Date).ToUniversalTime().ToString("o")
}

($report | ConvertTo-Json -Depth 20) | Set-Content -Path $reportPath -Encoding UTF8

Write-Output ("Session: {0}" -f $SessionDir)
Write-Output ("Aligned JSONL: {0}" -f $alignedJsonlPath)
Write-Output ("Aligned CSV: {0}" -f $alignedCsvPath)
Write-Output ("Report: {0}" -f $reportPath)
Write-Output ("Start offset (input - screen): {0} ms" -f $report.input_minus_screen_ms)
Write-Output ("Event count: {0}" -f $eventCount)
