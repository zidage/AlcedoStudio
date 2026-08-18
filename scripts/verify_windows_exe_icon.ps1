#requires -Version 5.1
<#
.SYNOPSIS
    Verify that a Windows EXE PE resource section embeds the committed ICO.

    Explorer, Start Menu, and desktop shortcuts read the EXE icon from the PE
    resource directory, not from the Qt RCC payload. Incremental MSVC builds
    used to keep a stale .res when only alcedo_icon.ico changed, so this check
    compares RT_ICON blobs against the ICO image payloads.
#>
param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [Parameter(Mandatory = $true)][string]$Ico
)

$ErrorActionPreference = 'Stop'

function Get-UInt16 {
    param([byte[]]$Bytes, [int]$Offset)
    return [BitConverter]::ToUInt16($Bytes, $Offset)
}

function Get-Sha256Hex {
    param([byte[]]$Bytes)
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        $hash = $sha.ComputeHash($Bytes)
        return ([BitConverter]::ToString($hash) -replace '-', '')
    } finally {
        $sha.Dispose()
    }
}

function Get-UInt32 {
    param([byte[]]$Bytes, [int]$Offset)
    return [BitConverter]::ToUInt32($Bytes, $Offset)
}

function Get-IcoImageHashes {
    param([string]$Path)
    $bytes = [IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -lt 6) {
        throw "ICO is too small: $Path"
    }
    $type = Get-UInt16 $bytes 2
    $count = Get-UInt16 $bytes 4
    if ($type -ne 1 -or $count -lt 1) {
        throw "Not a valid ICO ($type/$count): $Path"
    }

    $hashes = New-Object System.Collections.Generic.List[string]
    for ($i = 0; $i -lt $count; $i++) {
        $entry = 6 + ($i * 16)
        if (($entry + 16) -gt $bytes.Length) {
            throw "ICO directory is truncated: $Path"
        }
        $size = Get-UInt32 $bytes ($entry + 8)
        $offset = Get-UInt32 $bytes ($entry + 12)
        $end = [int64]$offset + [int64]$size
        if ($offset -lt 0 -or $size -lt 1 -or $end -gt $bytes.Length) {
            throw "ICO image $i is out of range: $Path"
        }
        $image = New-Object byte[] $size
        [Array]::Copy($bytes, $offset, $image, 0, $size)
        $hashes.Add((Get-Sha256Hex -Bytes $image))
    }
    return $hashes
}

function Convert-RvaToOffset {
    param(
        [uint32]$Rva,
        [System.Collections.Generic.List[object]]$Sections
    )
    foreach ($section in $Sections) {
        $start = [uint32]$section.VirtualAddress
        $span = [Math]::Max([uint32]$section.VirtualSize, [uint32]$section.SizeOfRawData)
        if ($Rva -ge $start -and $Rva -lt ($start + $span)) {
            return [int]($Rva - $start + [uint32]$section.PointerToRawData)
        }
    }
    throw "RVA 0x$($Rva.ToString('X')) is not in any PE section"
}

function Get-PeResourceDataEntries {
    param(
        [byte[]]$Bytes,
        [int]$ResourceOffset,
        [int]$DirectoryOffset,
        [System.Collections.Generic.List[object]]$Sections,
        [int]$Depth,
        [int]$TypeId
    )
    if ($Depth -gt 4) {
        throw "PE resource directory is nested too deeply"
    }
    $base = $ResourceOffset + $DirectoryOffset
    if (($base + 16) -gt $Bytes.Length) {
        throw "PE resource directory is truncated"
    }
    $named = Get-UInt16 $Bytes ($base + 12)
    $ids = Get-UInt16 $Bytes ($base + 14)
    $count = [int]$named + [int]$ids
    $entries = New-Object System.Collections.Generic.List[object]
    for ($i = 0; $i -lt $count; $i++) {
        $entryOffset = $base + 16 + ($i * 8)
        $nameOrId = Get-UInt32 $Bytes $entryOffset
        $offsetToData = Get-UInt32 $Bytes ($entryOffset + 4)
        $id = [int]($nameOrId -band 0x7FFFFFFF)
        $isDirectory = ($offsetToData -band 0x80000000) -ne 0
        $next = [int]($offsetToData -band 0x7FFFFFFF)
        $childTypeId = $TypeId
        if ($Depth -eq 0) {
            $childTypeId = $id
        }
        if ($isDirectory) {
            $child = Get-PeResourceDataEntries -Bytes $Bytes -ResourceOffset $ResourceOffset `
                -DirectoryOffset $next -Sections $Sections -Depth ($Depth + 1) `
                -TypeId $childTypeId
            foreach ($item in $child) {
                $entries.Add($item)
            }
        } else {
            $dataEntry = $ResourceOffset + $next
            if (($dataEntry + 16) -gt $Bytes.Length) {
                throw "PE resource data entry is truncated"
            }
            $dataRva = Get-UInt32 $Bytes $dataEntry
            $dataSize = Get-UInt32 $Bytes ($dataEntry + 4)
            $fileOffset = Convert-RvaToOffset -Rva $dataRva -Sections $Sections
            $end = [int64]$fileOffset + [int64]$dataSize
            if ($fileOffset -lt 0 -or $dataSize -lt 1 -or $end -gt $Bytes.Length) {
                throw "PE resource payload is out of range"
            }
            $payload = New-Object byte[] $dataSize
            [Array]::Copy($Bytes, $fileOffset, $payload, 0, $dataSize)
            $entries.Add([pscustomobject]@{
                TypeId = $childTypeId
                Id = $id
                Hash = (Get-Sha256Hex -Bytes $payload)
                Size = [int]$dataSize
            })
        }
    }
    return $entries
}

function Get-PeIconHashes {
    param([string]$Path)
    $bytes = [IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -lt 64 -or $bytes[0] -ne 0x4D -or $bytes[1] -ne 0x5A) {
        throw "Not a PE executable: $Path"
    }
    $peOffset = [int](Get-UInt32 $bytes 0x3C)
    if (($peOffset + 24) -gt $bytes.Length) {
        throw "PE header is truncated: $Path"
    }
    if ([Text.Encoding]::ASCII.GetString($bytes, $peOffset, 4) -ne "PE`0`0") {
        throw "Missing PE signature: $Path"
    }

    $coff = $peOffset + 4
    $sectionCount = Get-UInt16 $bytes ($coff + 2)
    $optionalSize = Get-UInt16 $bytes ($coff + 16)
    $optional = $coff + 20
    $magic = Get-UInt16 $bytes $optional
    if ($magic -eq 0x20B) {
        $dataDirectory = $optional + 112
    } elseif ($magic -eq 0x10B) {
        $dataDirectory = $optional + 96
    } else {
        throw "Unsupported optional-header magic 0x$($magic.ToString('X')): $Path"
    }
    $resourceDir = $dataDirectory + (2 * 8)
    if (($resourceDir + 8) -gt $bytes.Length) {
        throw "PE data directories are truncated: $Path"
    }
    $resourceRva = Get-UInt32 $bytes $resourceDir
    $resourceSize = Get-UInt32 $bytes ($resourceDir + 4)
    if ($resourceRva -eq 0 -or $resourceSize -eq 0) {
        throw "EXE has no PE resource directory: $Path"
    }

    $sectionTable = $optional + $optionalSize
    $sections = New-Object System.Collections.Generic.List[object]
    for ($i = 0; $i -lt $sectionCount; $i++) {
        $section = $sectionTable + ($i * 40)
        $sections.Add([pscustomobject]@{
            VirtualSize = Get-UInt32 $bytes ($section + 8)
            VirtualAddress = Get-UInt32 $bytes ($section + 12)
            SizeOfRawData = Get-UInt32 $bytes ($section + 16)
            PointerToRawData = Get-UInt32 $bytes ($section + 20)
        })
    }

    $resourceOffset = Convert-RvaToOffset -Rva $resourceRva -Sections $sections
    $entries = Get-PeResourceDataEntries -Bytes $bytes -ResourceOffset $resourceOffset `
        -DirectoryOffset 0 -Sections $sections -Depth 0 -TypeId 0
    $iconHashes = New-Object System.Collections.Generic.List[string]
    foreach ($entry in $entries) {
        # RT_ICON = 3. Explorer/Start Menu render these blobs, not Qt RCC.
        if ([int]$entry.TypeId -eq 3) {
            $iconHashes.Add($entry.Hash)
        }
    }
    if ($iconHashes.Count -lt 1) {
        throw "EXE has no RT_ICON resources: $Path"
    }
    return $iconHashes
}

if (-not (Test-Path -LiteralPath $Exe -PathType Leaf)) {
    throw "EXE not found: $Exe"
}
if (-not (Test-Path -LiteralPath $Ico -PathType Leaf)) {
    throw "ICO not found: $Ico"
}

$icoHashes = @(Get-IcoImageHashes -Path $Ico)
$exeHashes = @(Get-PeIconHashes -Path $Exe)
$missing = @($icoHashes | Where-Object { $exeHashes -notcontains $_ })
$extra = @($exeHashes | Where-Object { $icoHashes -notcontains $_ })

if ($missing.Count -gt 0 -or $extra.Count -gt 0) {
    throw @(
        "Windows EXE PE icon does not match $Ico",
        "  EXE: $Exe",
        "  ICO images: $($icoHashes.Count); PE RT_ICON images: $($exeHashes.Count)",
        "  Missing from EXE: $($missing.Count); extra in EXE: $($extra.Count)",
        "  Explorer/Start Menu will keep showing the stale PE resource."
    ) -join [Environment]::NewLine
}

Write-Host "[alcedo] Windows EXE PE icon matches $Ico ($($icoHashes.Count) images)" -ForegroundColor Green
