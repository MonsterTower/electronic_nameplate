param(
    [string]$Port = "COM5"
)

$ErrorActionPreference = "Stop"

try {
    Set-Location $PSScriptRoot
    . D:\esp\v6.0.2\esp-idf\export.ps1
    idf.py -p $Port -b 115200 flash monitor
}
finally {
    Read-Host "完成后按 Enter 关闭窗口"
}
