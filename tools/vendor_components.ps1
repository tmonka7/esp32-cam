<#
.SYNOPSIS
    Re-vendors the third-party components under components/.

.DESCRIPTION
    The project is built offline: esp32-camera and esp-modbus are committed to
    this repo rather than fetched from the ESP component registry. This script
    regenerates them and is the only thing here that needs network access.

    Both tags below are pinned deliberately:
      esp32-camera v2.0.15 - the last release with no external dependencies.
                             v2.1.x pulls in espressif/esp_jpeg, which would
                             mean vendoring a third component.
      esp-modbus   v1.0.9  - the newest 1.0.x tag. The 2.x line replaced the
                             mbc_master_* controller API used by app_modbus.c.

.EXAMPLE
    pwsh tools/vendor_components.ps1
#>

[CmdletBinding()]
param(
    [string]$CameraTag = 'v2.0.15',
    [string]$ModbusTag = 'v1.0.9'
)

$ErrorActionPreference = 'Stop'

$root       = Split-Path -Parent $PSScriptRoot
$components = Join-Path $root 'components'
$staging    = Join-Path ([System.IO.Path]::GetTempPath()) "vendor-$(Get-Random)"

# Development scaffolding that would otherwise more than double the tree size.
$dropDirs  = @('.git', '.github', '.gitlab', 'examples', 'test', 'test_apps', 'docs', 'tools')
$dropFiles = @('.gitignore', '.gitlab-ci.yml', 'pytest.ini', 'build_all.sh', 'component.mk')

function Import-Component {
    param(
        [Parameter(Mandatory)] [string]$Name,
        [Parameter(Mandatory)] [string]$Url,
        [Parameter(Mandatory)] [string]$Tag
    )

    $work = Join-Path $staging $Name
    Write-Host "fetching $Name $Tag"
    git clone --quiet --depth 1 --branch $Tag $Url $work
    if ($LASTEXITCODE -ne 0) {
        throw "git clone failed for $Name"
    }

    $sha = (git -C $work rev-parse HEAD).Trim()

    foreach ($d in $dropDirs) {
        $p = Join-Path $work $d
        if (Test-Path $p) { Remove-Item -Recurse -Force $p }
    }
    foreach ($f in $dropFiles) {
        $p = Join-Path $work $f
        if (Test-Path $p) { Remove-Item -Force $p }
    }

    # Provenance, so it is obvious later what these files are and where they
    # came from. Not consumed by the build.
    @(
        "component: $Name"
        "upstream:  $Url"
        "tag:       $Tag"
        "commit:    $sha"
        "vendored:  $(Get-Date -Format 'yyyy-MM-dd')"
        ""
        "Regenerate with tools/vendor_components.ps1 - do not edit by hand."
    ) | Set-Content -Encoding utf8 (Join-Path $work 'VENDORED.txt')

    $dest = Join-Path $components $Name
    if (Test-Path $dest) { Remove-Item -Recurse -Force $dest }
    Move-Item $work $dest

    $files = (Get-ChildItem -Recurse -File $dest).Count
    Write-Host "  -> components/$Name  ($files files, $($sha.Substring(0,8)))"
}

New-Item -ItemType Directory -Force -Path $staging, $components | Out-Null

try {
    Import-Component -Name 'esp32-camera' -Tag $CameraTag `
        -Url 'https://github.com/espressif/esp32-camera.git'
    Import-Component -Name 'esp-modbus' -Tag $ModbusTag `
        -Url 'https://github.com/espressif/esp-modbus.git'
}
finally {
    if (Test-Path $staging) { Remove-Item -Recurse -Force $staging -ErrorAction SilentlyContinue }
}

Write-Host "done - commit components/ to keep the build offline"
