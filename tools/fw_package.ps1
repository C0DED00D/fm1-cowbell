# Package fw/build (from tools/fw_build.sh) into an FM-1 update file, with fm1_open's own packer (sdk/Package.cs):
#   powershell -ExecutionPolicy Bypass -File tools\fw_package.ps1 [-Out fw\build\COWBELL.fwsc]
param([string]$Out = '')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$open = if ($env:FM1_OPEN) { $env:FM1_OPEN } else { Join-Path $root 'third_party\fm1_open' }   # fm1_open's checkout
$sdk = Join-Path $open 'sdk'
$gen = Join-Path $root 'fw\build\generated'
if (!$Out) { $Out = Join-Path $root 'fw\build\COWBELL.fwsc' }
$Out = [IO.Path]::GetFullPath($Out)
$elf = [IO.File]::ReadAllBytes((Join-Path $root 'fw\build\sdk.elf'))
if ([BitConverter]::ToUInt32($elf, 24) -ne 0x02000120) { throw 'Package entry must match the FM1 firmware entry (0x02000120).' }
$product = (Get-Content (Join-Path $gen 'product.txt')).Trim()
$image = New-Object IO.MemoryStream
foreach ($s in @('text', 'data', 'ram0_data', 'cache_ram_data')) {
    $b = [IO.File]::ReadAllBytes((Join-Path $gen "$s.bin")); $image.Write($b, 0, $b.Length)
}
Add-Type -Path (Join-Path $sdk 'Package.cs')
Push-Location $gen
try { $layout = [FM1.Package]::Create($image.ToArray(), (Join-Path $sdk 'tools\pack'), $Out, 536576, $product) }
finally { Pop-Location }
Write-Host "$Out ($product): image $($layout.FLASH_BIN_SIZE), VM $($layout.VM_REAL_SIZE), user $($layout.user_bytes) bytes"
