<#
.SYNOPSIS
    Rebuilds the native engine DLL when sources are newer, then runs the voxel
    demo in a release-like context against the live project folder - no export
    step, no debugger, optimized engine DLL.

.DESCRIPTION
    Step 1 - freshness: the release DLL in bin/ is compared against include/,
    src/ and CMakeLists.txt. If any source is newer (or the DLL is missing),
    build-release is configured if needed and rebuilt, and the freshly built
    artifact is copied into bin/. Skipped with -SkipBuild; forced with
    -ForceBuild.

    Step 2 - launch, in order of preference:

    1. The installed Windows *release export template*, pointed at the project
       folder with `--path` (release feature tag, no editor host). Official
       Godot templates are compiled with `disable_path_overrides=yes` and
       reject `--path`; this only works with a custom template built with
       `disable_path_overrides=no`.
    2. The installed editor binary, also with `--path`. This is the fallback
       on stock installations: it uses the `debug` feature tag and the editor
       host, but `voxel_engine.gdextension` maps both feature tags to the
       release DLL, so the native engine cost is unchanged.

    A true release run remains a full export (see running-prod.md, Step 3).

.PARAMETER GodotVersion
    Export-template version folder to use, e.g. '4.7.2.stable'. If it does not
    contain windows_release_x86_64.exe, the newest installed version that does
    is used instead.

.PARAMETER EditorExe
    Optional explicit path to a Godot editor binary to use as the fallback host.

.PARAMETER SkipBuild
    Do not check or rebuild the native DLL; launch whatever is in bin/.

.PARAMETER ForceBuild
    Rebuild the native DLL even when bin/ looks up to date.

.PARAMETER GodotArgs
    Any extra arguments are forwarded verbatim to the host (e.g. a scene path,
    `--resolution 1280x720`, `--headless`).

.EXAMPLE
    .\run-prod.ps1

.EXAMPLE
    .\run-prod.ps1 --resolution 1920x1080

.EXAMPLE
    .\run-prod.ps1 -SkipBuild --headless --quit
#>
[CmdletBinding()]
param(
    [string]$GodotVersion = '4.7.2.stable',
    [string]$EditorExe = '',
    [switch]$SkipBuild,
    [switch]$ForceBuild,

    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$GodotArgs
)

$ErrorActionPreference = 'Stop'

$projectRoot = $PSScriptRoot
$buildDir = Join-Path $projectRoot 'build-release'
$releaseDll = Join-Path $projectRoot 'bin\libvoxel_engine.windows.release.x86_64.dll'
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

function Find-CMake {
    $cmd = Get-Command cmake -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $local = Join-Path $env:LOCALAPPDATA 'CMake\bin\cmake.exe'
    if (Test-Path -LiteralPath $local) { return $local }
    return ''
}

function Test-ReleaseBuildStale {
    param([string]$Root, [string]$DllPath)

    if (-not (Test-Path -LiteralPath $DllPath)) { return $true }
    $dllTime = (Get-Item -LiteralPath $DllPath).LastWriteTimeUtc

    foreach ($dir in @((Join-Path $Root 'include'), (Join-Path $Root 'src'))) {
        if (-not (Test-Path -LiteralPath $dir)) { continue }
        $newer = Get-ChildItem -LiteralPath $dir -Recurse -File -Include *.hpp, *.h, *.cpp |
            Where-Object { $_.LastWriteTimeUtc -gt $dllTime } |
            Select-Object -First 1
        if ($newer) { return $true }
    }

    $cmakeLists = Join-Path $Root 'CMakeLists.txt'
    if ((Test-Path -LiteralPath $cmakeLists) -and
        (Get-Item -LiteralPath $cmakeLists).LastWriteTimeUtc -gt $dllTime) {
        return $true
    }
    return $false
}

function Invoke-ReleaseBuild {
    param([string]$Root, [string]$BuildDir, [string]$CMake, [string]$DllPath)

    if (-not (Test-Path -LiteralPath (Join-Path $BuildDir 'CMakeCache.txt'))) {
        $godotCpp = Join-Path (Split-Path -Parent $Root) 'Terrain3D\godot-cpp'
        if (-not (Test-Path -LiteralPath $godotCpp)) {
            Write-Error ("godot-cpp checkout not found at '$godotCpp'. " +
                         "Configure build-release manually (running-prod.md, Step 1).")
            exit 1
        }
        Write-Host "  build:    configuring build-release (template_release)..."
        & $CMake -B $BuildDir -DVOXEL_BUILD_GDEXTENSION=ON -DGODOTCPP_TARGET=template_release `
            -DGODOT_CPP_DIR=$($godotCpp -replace '\\', '/') -DVOXEL_BUILD_TESTS=OFF
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    }

    Write-Host "  build:    rebuilding release DLL..."
    & $CMake --build $BuildDir --config Release
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Release build failed - not launching."
        exit $LASTEXITCODE
    }

    $artifact = Join-Path $BuildDir 'bin\Release\libvoxel_engine.dll'
    if (Test-Path -LiteralPath $artifact) {
        $destTime = if (Test-Path -LiteralPath $DllPath) { (Get-Item -LiteralPath $DllPath).LastWriteTimeUtc } else { [datetime]::MinValue }
        if ((Get-Item -LiteralPath $artifact).LastWriteTimeUtc -gt $destTime) {
            Copy-Item -LiteralPath $artifact -Destination $DllPath -Force
        }
    }
}

function Find-EditorExe {
    param([string]$Version)

    $ver = $Version -replace '\.stable$', ''
    $exeName = "Godot_v$ver-stable_win64.exe"
    $roots = @(
        (Join-Path $env:USERPROFILE 'bin'),
        (Join-Path $env:USERPROFILE 'Documents'),
        (Join-Path $env:USERPROFILE 'Downloads'),
        'C:\Program Files'
    )
    foreach ($root in $roots) {
        if (-not (Test-Path -LiteralPath $root)) { continue }
        $found = Get-ChildItem -LiteralPath $root -Recurse -Depth 3 -Filter $exeName -File -ErrorAction SilentlyContinue |
            Select-Object -First 1
        if ($found) { return $found.FullName }
    }
    return ''
}

Write-Host "Running release build of '$projectRoot'"
if ($GodotArgs.Count -gt 0) {
    Write-Host ("  args:     " + ($GodotArgs -join ' '))
}

if ($SkipBuild) {
    Write-Host "  dll:      build check skipped (-SkipBuild)"
} else {
    $stale = $ForceBuild -or (Test-ReleaseBuildStale -Root $projectRoot -DllPath $releaseDll)
    if ($stale) {
        $cmake = Find-CMake
        if (-not $cmake) {
            Write-Error ("cmake not found on PATH or at '$env:LOCALAPPDATA\CMake\bin'; " +
                         "cannot rebuild the release DLL. Install CMake or run with -SkipBuild.")
            exit 1
        }
        Invoke-ReleaseBuild -Root $projectRoot -BuildDir $buildDir -CMake $cmake -DllPath $releaseDll
        Write-Host "  dll:      rebuilt $(Split-Path -Leaf $releaseDll)"
    } else {
        Write-Host "  dll:      up to date (use -ForceBuild to rebuild anyway)"
    }
}

if (Test-Path -LiteralPath $templateExe) {
    Write-Host "  template: $templateExe  ($GodotVersion)"

    $forward = @('--path', $projectRoot)
    if ($GodotArgs.Count -gt 0) { $forward += $GodotArgs }

    $stderrFile = [System.IO.Path]::GetTempFileName()
    $proc = Start-Process -FilePath $templateExe -ArgumentList $forward `
        -NoNewWindow -Wait -PassThru -RedirectStandardError $stderrFile
    $stderrText = Get-Content -LiteralPath $stderrFile -Raw -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $stderrFile -ErrorAction SilentlyContinue

    if ($proc.ExitCode -eq 0) {
        exit 0
    }
    if ($stderrText -and $stderrText -match 'path overrides') {
        Write-Host "  note:     template is compiled with disable_path_overrides=yes (official builds);"
        Write-Host "            falling back to the editor host - the native engine DLL is still the release build."
    } else {
        if ($stderrText) { Write-Host $stderrText.TrimEnd() }
        exit $proc.ExitCode
    }
} else {
    Write-Host "  template: none found under '$templatesRoot' - using the editor host."
}

if (-not $EditorExe) {
    $EditorExe = Find-EditorExe -Version $GodotVersion
}
if (-not $EditorExe -or -not (Test-Path -LiteralPath $EditorExe)) {
    Write-Error ("No Godot editor binary found for version '$GodotVersion'. " +
                 "Pass -EditorExe <path>, or install export templates and run a full export (running-prod.md, Step 3).")
    exit 1
}

Write-Host "  editor:   $EditorExe"
$forward = @('--path', $projectRoot)
if ($GodotArgs.Count -gt 0) { $forward += $GodotArgs }
$proc = Start-Process -FilePath $EditorExe -ArgumentList $forward -Wait -PassThru
exit $proc.ExitCode
