param(
    [string[]]$Shots = @(),
    [switch]$All,
    [string]$GodotExe = ""
)

$ErrorActionPreference = "Stop"
$ProjectRoot = Split-Path -Parent $PSScriptRoot
$capture = Join-Path $PSScriptRoot "capture_trailer.ps1"
$args = @("-Previs", "-OutputRoot", (Join-Path $ProjectRoot "captures\previs"))
if ($All -or $Shots.Count -eq 0) {
    $args += "-All"
} else {
    $args += "-Shots"
    $args += $Shots
}
if ($GodotExe) {
    $args += @("-GodotExe", $GodotExe)
}
& $capture @args
