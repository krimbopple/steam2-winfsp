param(
    [string]$Drive = "R:"
)

$ErrorActionPreference = "Stop"
$requiredFiles = @(
    "hl2.exe",
    "bin\engine.dll",
    "bin\launcher.dll",
    "bin\FileSystem_Steam.dll",
    "cstrike\bin\client.dll",
    "cstrike\bin\server.dll",
    "cstrike\maps\de_dust.bsp",
    "cstrike\scripts\gameinfo.txt"
)

$results = foreach ($relativePath in $requiredFiles) {
    $path = Join-Path $Drive $relativePath
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing required projected file: $relativePath"
    }
    $item = Get-Item -LiteralPath $path
    $hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
    [pscustomobject]@{
        Path = $relativePath
        Size = $item.Length
        Sha256 = $hash
    }
}

$sanitizedReslist = Join-Path $Drive "reslists\Counter-Strike Source\de_dust.lst"
if (-not (Test-Path -LiteralPath $sanitizedReslist -PathType Leaf)) {
    throw "Sanitized historical reslist path is missing"
}

$ramFile = Join-Path $Drive "cstrike\cfg\s2fs_ram_test.cfg"
[System.IO.File]::WriteAllText($ramFile, "s2fs_ephemeral 1`n")
if ([System.IO.File]::ReadAllText($ramFile) -ne "s2fs_ephemeral 1`n") {
    throw "RAM-backed config write mismatch"
}
Remove-Item -LiteralPath $ramFile
if (Test-Path -LiteralPath $ramFile) {
    throw "RAM-backed config delete failed"
}

[pscustomobject]@{
    RequiredFiles = $results
    SanitizedReslist = $sanitizedReslist
    RamWriteDeletePassed = $true
} | ConvertTo-Json -Depth 4 -Compress
