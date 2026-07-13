param(
    [string]$Port = "COM5"
)

$ErrorActionPreference = "Stop"
$projectDirectory = Join-Path $PSScriptRoot "hardware_bringup"
$idfExportScript = "D:\esp\v6.0.2\esp-idf\export.ps1"

if (-not (Test-Path -LiteralPath $projectDirectory)) {
    throw "Hardware project directory was not found: $projectDirectory"
}
if (-not (Test-Path -LiteralPath $idfExportScript)) {
    throw "ESP-IDF export script was not found: $idfExportScript"
}

Set-Location -LiteralPath $projectDirectory
. $idfExportScript

Write-Host "Monitoring $Port. Press Ctrl+] to exit."
idf.py -p $Port monitor

Read-Host "Monitor ended. Press Enter to close this window"
