param(
    [string]$Timeline = "trailer/edit_60.json",
    [string]$Captures = "captures/raw",
    [switch]$Preview,
    [switch]$Master,
    [switch]$Vertical,
    [switch]$All,
    [string]$BlenderExe = "C:\Program Files\Blender Foundation\Blender 5.2\blender.exe"
)

$ErrorActionPreference = "Stop"
$ProjectRoot = Split-Path -Parent $PSScriptRoot
$Editor = Join-Path $PSScriptRoot "edit_trailer.py"
if (-not (Test-Path -LiteralPath $BlenderExe)) {
    throw "Blender not found at $BlenderExe"
}

function Render-One {
    param([string]$TimelinePath, [switch]$DoPreview, [switch]$DoMaster, [switch]$DoVertical)
    $timelineAbs = if ([System.IO.Path]::IsPathRooted($TimelinePath)) { $TimelinePath } else { Join-Path $ProjectRoot $TimelinePath }
    if ($DoPreview) {
        Write-Host "[Trailer] Rendering preview for $TimelinePath"
        & $BlenderExe -b -P $Editor -- --timeline $timelineAbs --captures $Captures --preview
        if ($LASTEXITCODE -ne 0) { throw "Blender preview failed for $TimelinePath" }
    }
    if ($DoMaster) {
        Write-Host "[Trailer] Rendering ProRes master for $TimelinePath"
        & $BlenderExe -b -P $Editor -- --timeline $timelineAbs --captures $Captures
        if ($LASTEXITCODE -ne 0) { throw "Blender master failed for $TimelinePath" }
    }
    if ($DoVertical) {
        Write-Host "[Trailer] Rendering vertical preview for $TimelinePath"
        & $BlenderExe -b -P $Editor -- --timeline $timelineAbs --captures $Captures --preview --vertical
        if ($LASTEXITCODE -ne 0) { throw "Blender vertical failed for $TimelinePath" }
    }
}

if ($All) {
    foreach ($tl in @("trailer/edit_60.json", "trailer/edit_75.json", "trailer/edit_120.json")) {
        Render-One -TimelinePath $tl -DoPreview -DoMaster -DoVertical
    }
} else {
    if (-not $Preview -and -not $Master -and -not $Vertical) {
        $Preview = $true
    }
    Render-One -TimelinePath $Timeline -DoPreview:$Preview -DoMaster:$Master -DoVertical:$Vertical
}

Write-Host "[Trailer] Render pass complete. Outputs are under captures/edit."
