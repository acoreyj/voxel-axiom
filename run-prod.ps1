<#
.SYNOPSIS
    Runs the voxel demo through the Godot *release* export template against the
    live project folder - i.e. release feature tag, optimized engine, no
    debugger - without doing a full export.

.DESCRIPTION
    Godot's F5 always runs the project through the editor's devel host with the
    `debug` feature tag; there is no "run in release" editor mode. This launcher
    points the installed Windows release export template at the project directory
    (it reads res:// straight from source, so no export step is needed).

    The GDExtension itself is already the release build in both slots, so this
    script only removes the remaining gap: the editor host, the debugger, and the
    debug feature tag.

.PARAMETER GodotVersion
    Export-template version folder to use, e.g. '4.7.2.stable'. If it does not
    contain windows_release_x86_64.exe, the newest installed version that does is
    used instead.

.PARAMETER GodotArgs
    Any extra arguments are forwarded verbatim to the template (e.g. a scene
    path, `--resolution 1280x720`, `--headless`).

.EXAMPLE
    .\run-prod.ps1

.EXAMPLE
    .\run-prod.ps1 --resolution 1920x1080
#>
[CmdletBinding()]
param(
    [string]$GodotVersion = '4.7.2.stable',

    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$GodotArgs
)

$ErrorActionPreference = 'Stop'

$projectRoot = $PSScriptRoot
$templatesRoot = Join-Path $env:APPDATA 'Godot\export_templates'
$templateExe = Join-Path (Join-Path $templatesRoot $GodotVersion) 'windows_release_x86_64.exe'

if (-not (Test-Path -LiteralPath $templateExe)) {
    $candidates = Get-ChildItem -LiteralPath $templatesRoot -Directory -ErrorAction SilentlyContinue |
        Where-Object { Test-Path (Join-Path $_.FullName 'windows_release_x86_64.exe') } |
        Sort-Object Name -Descending
    if ($candidates) {
        $templateExe = Join-Path $candidates[0].FullName 'windows_release_x86_64.exe'
        $GodotVersion = $candidates[0].Name
    }
}

if (-not (Test-Path -LiteralPath $templateExe)) {
    Write-Error ("No Godot Windows release export template found under '$templatesRoot'. " +
                 "Install them via the editor: Editor -> Manage Export Templates... -> Download and Install.")
    exit 1
}

Write-Host "Running release build of '$projectRoot'"
Write-Host "  template: $templateExe  ($GodotVersion)"
if ($GodotArgs.Count -gt 0) {
    Write-Host ("  args:     " + ($GodotArgs -join ' '))
}

& $templateExe --path $projectRoot @GodotArgs
exit $LASTEXITCODE
