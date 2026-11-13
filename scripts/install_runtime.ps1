param(
    [string]$RuntimeDir = "$env:LOCALAPPDATA\CorridorKey2OFX\runtime",
    [string]$Revision = "485f5e9",
    [switch]$Latest
)
$ErrorActionPreference = "Stop"

function Need($name, $hint) {
    if (-not (Get-Command $name -ErrorAction SilentlyContinue)) { throw "$name not found. $hint" }
}

Need git "Install Git for Windows: https://git-scm.com/download/win"
Need py "Install Python 3.12 from python.org (it installs the 'py' launcher)."
& py -3.12 -c "import sys" 2>$null
if ($LASTEXITCODE -ne 0) { throw "Python 3.12 not found (py -3.12). CorridorKey 2.0 needs numpy 1.x, which has no wheels for 3.13+." }
if (-not (Get-Command nvidia-smi -ErrorAction SilentlyContinue)) { Write-Warning "nvidia-smi not found: CorridorKey 2.0 needs an NVIDIA RTX 30 series or newer GPU." }

New-Item -ItemType Directory -Force $RuntimeDir | Out-Null
$ck2 = Join-Path $RuntimeDir "ck2"
if (-not (Test-Path (Join-Path $ck2 ".git"))) {
    git clone https://github.com/nikopueringer/CorridorKey2.0 $ck2
    if ($LASTEXITCODE -ne 0) { throw "git clone failed." }
}
Push-Location $ck2
git fetch --quiet origin
if ($Latest) { git checkout --quiet origin/HEAD } else { git checkout --quiet $Revision }
if ($LASTEXITCODE -ne 0) { throw "git checkout failed." }
Pop-Location

$venv = Join-Path $RuntimeDir "venv"
$python = Join-Path $venv "Scripts\python.exe"
if (-not (Test-Path $python)) { & py -3.12 -m venv $venv }
& $python -m pip install --quiet --upgrade pip
& $python -m pip install --quiet torch==2.9.1 torchvision==0.24.1 --index-url https://download.pytorch.org/whl/cu128
if ($LASTEXITCODE -ne 0) { throw "PyTorch install failed." }
& $python -m pip install --quiet -r (Join-Path $ck2 "requirements.txt")
if ($LASTEXITCODE -ne 0) { throw "requirements install failed." }

Push-Location $ck2
& $python scripts\download_weights.py
$code = $LASTEXITCODE
Pop-Location
if ($code -ne 0) { throw "Weight download failed." }

& $python -c "import torch; assert torch.cuda.is_available(), 'CUDA is not available to PyTorch'; print('CUDA OK:', torch.cuda.get_device_name(0))"
if ($LASTEXITCODE -ne 0) { throw "PyTorch cannot see the GPU." }

$default = "$env:LOCALAPPDATA\CorridorKey2OFX\runtime"
if ((Resolve-Path $RuntimeDir).Path -ne $default) {
    Set-Content -Encoding utf8 -Path "$env:LOCALAPPDATA\CorridorKey2OFX\runtime.txt" -Value (Resolve-Path $RuntimeDir).Path
}
Write-Host "CorridorKey 2.0 runtime ready: $RuntimeDir"
