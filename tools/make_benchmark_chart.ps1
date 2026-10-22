param(
    [string]$ResultsPath = "",
    [string]$OutputPath = "",
    [switch]$RequireComplete
)

$ErrorActionPreference = "Stop"
$ProjectRoot = Split-Path -Parent $PSScriptRoot
if (-not $ResultsPath) {
    $ResultsPath = Join-Path $ProjectRoot "voxel_benchmark_results.json"
}
if (-not $OutputPath) {
    $OutputPath = Join-Path $ProjectRoot "trailer\assets\benchmark_chart.png"
}
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $OutputPath) | Out-Null

$data = Get-Content -LiteralPath $ResultsPath -Raw | ConvertFrom-Json
$status = [string]$data.status
$complete = $status -like "*Completed*" -and @($data.presets).Count -ge 5
if ($RequireComplete -and -not $complete) {
    throw "Benchmark results are not a complete five-preset run (status='$status'). Rerun the full suite."
}

Add-Type -AssemblyName System.Drawing
$W = 3840
$H = 2160
$bmp = New-Object System.Drawing.Bitmap($W, $H)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
$g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAliasGridFit
$g.Clear([System.Drawing.Color]::FromArgb(8, 11, 15))

$bg = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(8, 11, 15))
$white = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(235, 245, 240))
$muted = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(130, 150, 150))
$green = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(105, 225, 165))
$greenDim = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(44, 115, 88))
$orange = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 142, 64))
$line = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(35, 48, 54), 3)
$fontH = New-Object System.Drawing.Font("Bahnschrift", 92, [System.Drawing.FontStyle]::Bold)
$fontS = New-Object System.Drawing.Font("Consolas", 34)
$fontLabel = New-Object System.Drawing.Font("Bahnschrift", 40)
$fontValue = New-Object System.Drawing.Font("Consolas", 40, [System.Drawing.FontStyle]::Bold)
$fontWarn = New-Object System.Drawing.Font("Bahnschrift", 64, [System.Drawing.FontStyle]::Bold)

$g.DrawString("BENCHMARK SUITE", $fontH, $white, 140, 120)
$hw = $data.hardware
$hwLine = "{0} | {1} | {2} | Godot {3}" -f $hw.cpu_name, $hw.gpu_name, $hw.window_resolution, $hw.godot_version
$g.DrawString($hwLine, $fontS, $muted, 146, 260)
$g.DrawString("5 presets | pristine undo_all() reset per pass | 8 deterministic edits", $fontS, $green, 146, 320)

if (-not $complete) {
    $g.DrawString("ABORTED RUN - RERUN FULL BENCHMARK", $fontWarn, $orange, 146, 400)
}

$presets = @($data.presets)
$maxFps = 1.0
foreach ($p in $presets) {
    $maxFps = [math]::Max($maxFps, [double]$p.overall.avg_fps)
}
$scaleMax = [math]::Ceiling($maxFps / 100.0) * 100.0
$left = 760
$right = 3540
$top = 620
$rowH = 210
$barH = 58
$plotW = $right - $left

for ($i = 0; $i -le 5; $i++) {
    $x = $left + ($plotW * $i / 5.0)
    $g.DrawLine($line, [int]$x, $top - 40, [int]$x, $top + ($rowH * 5) + 20)
    $val = [int]($scaleMax * $i / 5.0)
    $g.DrawString($val.ToString(), $fontS, $muted, [int]($x - 22), $top - 95)
}

for ($i = 0; $i -lt $presets.Count; $i++) {
    $p = $presets[$i]
    $y = $top + ($i * $rowH)
    $name = [string]$p.preset_name
    $avg = [double]$p.overall.avg_fps
    $low = [double]$p.overall.p1_low_fps
    $g.DrawString($name, $fontLabel, $white, 146, $y + 8)
    $settings = "view {0} / prefetch {1} / {2} ms" -f $p.settings.view_distance_chunks, $p.settings.prefetch_distance_chunks, $p.settings.upload_budget_ms
    $g.DrawString($settings, $fontS, $muted, 146, $y + 62)

    $avgW = [int]($plotW * ($avg / $scaleMax))
    $lowW = [int]($plotW * ($low / $scaleMax))
    $g.FillRectangle($greenDim, $left, $y + 24, $avgW, $barH)
    $g.FillRectangle($green, $left, $y + 24, $lowW, $barH)
    $g.DrawString(("{0:N0} FPS avg   {1:N0} FPS 1% low" -f $avg, $low), $fontValue, $white, $left + 18, $y + 92)
}

$g.DrawString("GREEN: 1% LOW   |   DIM GREEN: AVERAGE FPS", $fontS, $muted, 146, 1840)
$g.DrawString("Numbers are hardware- and resolution-specific. Chart reads the latest completed JSON.", $fontS, $muted, 146, 1905)
$g.DrawString("voxel_benchmark_results.json", $fontS, $green, 146, 2010)

$bmp.Save($OutputPath, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose()
$bmp.Dispose()
Write-Host "[Trailer] Benchmark chart written to $OutputPath (complete=$complete, status=$status)"
