[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet("x64", "arm64")]
    [string] $TargetArchitecture
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if (-not $IsWindows) {
    throw "MSVC setup requires Windows"
}
if (-not $env:GITHUB_ENV) {
    throw "GITHUB_ENV is not available"
}

$vswhere = Join-Path `
    ${env:ProgramFiles(x86)} `
    "Microsoft Visual Studio/Installer/vswhere.exe"
if (-not (Test-Path -LiteralPath $vswhere)) {
    throw "vswhere.exe was not found at $vswhere"
}

$requiredComponents = @("Microsoft.VisualStudio.Component.VC.Tools.x86.x64")
if ($TargetArchitecture -eq "arm64") {
    $requiredComponents += "Microsoft.VisualStudio.Component.VC.Tools.ARM64"
}
$installationPaths = @(
    & $vswhere `
        -latest `
        -products "*" `
        -prerelease `
        -requires $requiredComponents `
        -property installationPath
)
if ($LASTEXITCODE -ne 0 -or $installationPaths.Count -eq 0) {
    throw "Visual Studio with C++ tools was not found"
}

$installationPath = $installationPaths[0].Trim()
$vcvarsall = Join-Path $installationPath "VC/Auxiliary/Build/vcvarsall.bat"
if (-not (Test-Path -LiteralPath $vcvarsall)) {
    throw "vcvarsall.bat was not found at $vcvarsall"
}

$vcvarsArchitecture = @{
    x64 = "amd64"
    arm64 = "amd64_arm64"
}[$TargetArchitecture]

$initialEnvironment = [System.Collections.Generic.Dictionary[string, string]]::new(
    [System.StringComparer]::OrdinalIgnoreCase
)
Get-ChildItem Env: | ForEach-Object {
    $initialEnvironment[$_.Name] = $_.Value
}

$command = "call `"$vcvarsall`" $vcvarsArchitecture >nul && set"
$environmentLines = @(& $env:ComSpec /d /c $command)
if ($LASTEXITCODE -ne 0) {
    throw "vcvarsall.bat failed for $vcvarsArchitecture"
}

$exported = 0
foreach ($line in $environmentLines) {
    $separator = $line.IndexOf("=")
    if ($separator -le 0) {
        continue
    }

    $name = $line.Substring(0, $separator)
    $value = $line.Substring($separator + 1)
    $hadInitialValue = $initialEnvironment.ContainsKey($name)
    $initialValue = if ($hadInitialValue) { $initialEnvironment[$name] } else { $null }
    if ($hadInitialValue -and $initialValue -ceq $value) {
        continue
    }

    [System.Environment]::SetEnvironmentVariable($name, $value, "Process")
    Add-Content -LiteralPath $env:GITHUB_ENV -Value "$name=$value" -Encoding utf8
    $exported++
}

if ($exported -eq 0) {
    throw "vcvarsall.bat did not change the environment"
}
if ($env:VSCMD_ARG_TGT_ARCH -ne $TargetArchitecture) {
    throw "Expected MSVC target $TargetArchitecture, got $env:VSCMD_ARG_TGT_ARCH"
}

$tools = @("cl.exe", "link.exe", "rc.exe")
foreach ($tool in $tools) {
    if (-not (Get-Command $tool -CommandType Application -ErrorAction SilentlyContinue)) {
        throw "$tool is not available after MSVC setup"
    }
}

Write-Host (
    "Configured MSVC {0} and Windows SDK {1} for {2}" -f `
        $env:VCToolsVersion.Trim(), `
        $env:WindowsSDKVersion.TrimEnd([char] "\"), `
        $TargetArchitecture
)
