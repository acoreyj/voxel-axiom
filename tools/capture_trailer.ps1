param(
    [string[]]$Shots = @(),
    [switch]$All,
    [switch]$Previs,
    [string]$GodotExe = "",
    [string]$OutputRoot = "",
    [switch]$KeepOverride,
    [bool]$TrimPreRoll = $true,
    [switch]$SkipExisting
)

$ErrorActionPreference = "Stop"
$ProjectRoot = Split-Path -Parent $PSScriptRoot
$ManifestPath = Join-Path $ProjectRoot "trailer\shots.json"
$Manifest = Get-Content -LiteralPath $ManifestPath -Raw | ConvertFrom-Json

if (-not $OutputRoot) {
    $OutputRoot = if ($Previs) { Join-Path $ProjectRoot "captures\previs" } else { Join-Path $ProjectRoot "captures\raw" }
}
New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null

if (-not $GodotExe) {
    $candidates = @(
        "C:\Users\kuroi\Documents\Godot_v4.7.2-stable_win64.exe\Godot_v4.7.2-stable_win64_console.exe",
        "C:\Program Files (x86)\Godot_v4.7.2-stable_mono_win64\Godot_v4.7.2-stable_mono_win64_console.exe"
    )
    $GodotExe = $candidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
}
if (-not $GodotExe -or -not (Test-Path -LiteralPath $GodotExe)) {
    throw "Godot console executable not found. Pass -GodotExe <path>."
}

$shotIds = @()
if ($All) {
    $shotIds = @($Manifest.shots.PSObject.Properties.Name)
} elseif ($Shots.Count -gt 0) {
    $shotIds = $Shots
} else {
    throw "Pass -All or -Shots <id[,id...]>."
}

foreach ($shotId in $shotIds) {
    $shotProp = $Manifest.shots.PSObject.Properties[$shotId]
    if ($null -eq $shotProp) {
        throw "Unknown shot id: $shotId"
    }
    $shot = $shotProp.Value
    $fps = if ($shot.fps) { [int]$shot.fps } else { [int]$Manifest.fps }
    $duration = [double]$shot.duration
    $quitFrames = [int][math]::Ceiling(($duration + 180.0) * $fps)

    $captureDir = Join-Path $OutputRoot $shotId
    if ($SkipExisting -and (Test-Path -LiteralPath (Join-Path $captureDir "marker.txt"))) {
        $existing = @(Get-ChildItem -LiteralPath $captureDir -Filter "*.png" -ErrorAction SilentlyContinue)
        if ($existing.Count -gt 0) {
            Write-Host "[Trailer] Skipping $shotId ($($existing.Count) frames already captured)"
            continue
        }
    }
    if (Test-Path -LiteralPath $captureDir) {
        Remove-Item -LiteralPath $captureDir -Recurse -Force
    }
    New-Item -ItemType Directory -Force -Path $captureDir | Out-Null

    $saveDir = if ($shotId -like "fly_persist*") {
        Join-Path $ProjectRoot "trailer_saves\persist_pair"
    } else {
        Join-Path $ProjectRoot "trailer_saves\$shotId"
    }
    if ($shotId -eq "fly_persist_a" -or $shotId -notlike "fly_persist*") {
        if (Test-Path -LiteralPath $saveDir) {
            Remove-Item -LiteralPath $saveDir -Recurse -Force
        }
    }
    New-Item -ItemType Directory -Force -Path $saveDir | Out-Null

    $moviePath = Join-Path $captureDir "$shotId.png"
    $logPath = Join-Path $captureDir "capture.log"
    $godotArgs = @(
        "--path", $ProjectRoot,
        "--write-movie", $moviePath,
        "--fixed-fps", $fps,
        "--disable-vsync",
        "--quit-after", $quitFrames
    )
    if ($shotId -eq "debug_xray") {
        $godotArgs += @("--debug-collisions", "--debug-navigation")
    }
    $godotArgs += @("--", "--trailer-shot=$shotId", "--trailer-save-dir=$saveDir")

    $overridePath = Join-Path $ProjectRoot "override.cfg"
    $overrideBackup = "$overridePath.trailer-backup"
    $hadOverride = Test-Path -LiteralPath $overridePath
    if ($hadOverride) {
        Move-Item -LiteralPath $overridePath -Destination $overrideBackup -Force
    }
    try {
        if ($Previs) {
            Copy-Item -LiteralPath (Join-Path $ProjectRoot "trailer\previs_override.cfg") -Destination $overridePath -Force
        }
        Write-Host "[Trailer] Capturing $shotId at $fps fps -> $captureDir"
        $previousEap = $ErrorActionPreference
        $ErrorActionPreference = "Continue"
        & $GodotExe @godotArgs 2>&1 | Tee-Object -FilePath $logPath
        $code = $LASTEXITCODE
        $ErrorActionPreference = $previousEap
        if ($code -ne 0) {
            throw "Godot exited with code $code for $shotId"
        }
    } finally {
        if (-not $KeepOverride) {
            if (Test-Path -LiteralPath $overridePath) {
                Remove-Item -LiteralPath $overridePath -Force
            }
            if ($hadOverride -and (Test-Path -LiteralPath $overrideBackup)) {
                Move-Item -LiteralPath $overrideBackup -Destination $overridePath -Force
            }
        }
    }

    $pngs = @(Get-ChildItem -LiteralPath $captureDir -Filter "*.png" | Sort-Object Name)
    $markerPath = Join-Path $saveDir "marker.txt"
    $marker = if (Test-Path -LiteralPath $markerPath) { (Get-Content -LiteralPath $markerPath -TotalCount 1) } else { "missing" }
    if (Test-Path -LiteralPath $markerPath) {
        Copy-Item -LiteralPath $markerPath -Destination (Join-Path $captureDir "marker.txt") -Force
    }
    if ($pngs.Count -eq 0) {
        throw "No frames were written for $shotId"
    }

    if ($TrimPreRoll -and $marker -ne "missing") {
        $markerInt = [int]$marker
        $preMarker = @($pngs | Where-Object {
            $m = [regex]::Match($_.Name, '(\d+)\.png$')
            $m.Success -and [int]$m.Groups[1].Value -lt $markerInt
        })
        foreach ($frame in $preMarker) {
            Remove-Item -LiteralPath $frame.FullName -Force
        }
        $pngs = @(Get-ChildItem -LiteralPath $captureDir -Filter "*.png" | Sort-Object Name)
        $kept = @($pngs | Where-Object {
            $m = [regex]::Match($_.Name, '(\d+)\.png$')
            $m.Success -and [int]$m.Groups[1].Value -ge $markerInt
        } | Sort-Object {
            $m = [regex]::Match($_.Name, '(\d+)\.png$')
            [int]$m.Groups[1].Value
        })
        if ($kept.Count -gt 0) {
            $newIndex = 0
            foreach ($frame in $kept) {
                $target = Join-Path $captureDir ("{0}{1:D8}.png" -f $shotId, $newIndex)
                if ($frame.FullName -ne $target) {
                    Move-Item -LiteralPath $frame.FullName -Destination $target -Force
                }
                $newIndex++
            }
            $markerOut = Join-Path $captureDir "marker.txt"
            @(
                "0",
                $shotId,
                $fps,
                ([double]$shot.duration).ToString("0.###", [System.Globalization.CultureInfo]::InvariantCulture),
                ([double]$shot.lead).ToString("0.###", [System.Globalization.CultureInfo]::InvariantCulture),
                ([double]$shot.tail).ToString("0.###", [System.Globalization.CultureInfo]::InvariantCulture)
            ) | Set-Content -LiteralPath $markerOut
            Copy-Item -LiteralPath $markerOut -Destination (Join-Path $saveDir "marker.txt") -Force
            $pngs = @(Get-ChildItem -LiteralPath $captureDir -Filter "*.png" | Sort-Object Name)
            $marker = "0"
        }
    }

    Write-Host ("[Trailer] {0}: {1} frames, marker={2}, first={3}, last={4}" -f `
        $shotId, $pngs.Count, $marker, $pngs[0].Name, $pngs[$pngs.Count - 1].Name)
}

Write-Host "[Trailer] Capture pass complete."
