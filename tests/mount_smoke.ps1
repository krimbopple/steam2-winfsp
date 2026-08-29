param(
    [string]$Drive = "R:"
)

$ErrorActionPreference = "Stop"
$root = "$Drive\cstrike"
$client = Join-Path $root "bin\client.dll"
$credits = Join-Path $root "credits.txt"

if (-not (Test-Path -LiteralPath $client -PathType Leaf)) {
    throw "Missing projected client.dll"
}
if (-not (Test-Path -LiteralPath $credits -PathType Leaf)) {
    throw "Missing projected credits.txt"
}

$clientInfo = Get-Item -LiteralPath $client
$clientHash = (Get-FileHash -LiteralPath $client -Algorithm SHA256).Hash
$originalCredits = [System.IO.File]::ReadAllBytes($credits)
$sha256 = [System.Security.Cryptography.SHA256]::Create()
try {
    $originalCreditsHash = ([BitConverter]::ToString(
        $sha256.ComputeHash($originalCredits))).Replace("-", "")
} finally {
    $sha256.Dispose()
}

$cfgDirectory = Join-Path $root "cfg"
$newFile = Join-Path $cfgDirectory "s2fs-write-test.cfg"
$renamedFile = Join-Path $cfgDirectory "s2fs-write-renamed.cfg"
New-Item -ItemType Directory -Path $cfgDirectory -Force | Out-Null
[System.IO.File]::WriteAllText($newFile, "first`n")
[System.IO.File]::AppendAllText($newFile, "second`n")
if ([System.IO.File]::ReadAllText($newFile) -ne "first`nsecond`n") {
    throw "RAM-backed new-file content mismatch"
}
Move-Item -LiteralPath $newFile -Destination $renamedFile
if (-not (Test-Path -LiteralPath $renamedFile -PathType Leaf)) {
    throw "RAM-backed rename failed"
}

[System.IO.File]::WriteAllText($credits, "overlay-write")
$stream = [System.IO.File]::Open($credits, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Write)
try {
    $stream.SetLength(7)
} finally {
    $stream.Dispose()
}
if ([System.IO.File]::ReadAllText($credits) -ne "overlay") {
    throw "Archive-backed copy-on-write/truncate mismatch"
}

Remove-Item -LiteralPath $renamedFile
Remove-Item -LiteralPath $cfgDirectory

[pscustomobject]@{
    ClientSize = $clientInfo.Length
    ClientSha256 = $clientHash
    OriginalCreditsSha256 = $originalCreditsHash
    OverlayCredits = [System.IO.File]::ReadAllText($credits)
} | ConvertTo-Json -Compress
