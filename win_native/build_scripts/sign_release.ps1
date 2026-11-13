# SPDX-License-Identifier: MIT
<#
.SYNOPSIS
Signs the package of a release, so that the program's own update accepts it.

.DESCRIPTION
The update (win_native/src/uxplay_update_install) installs a package only when
its signature fits the public key the program carries: ECDSA P-256 over the
SHA-256 of the zip.  This script makes that signature with the private key,
which is kept outside the source tree and never published.

  sign_release.ps1 -KeyFile <key.pem> -NewKey
      makes a key pair, writes the private key to <key.pem> (readable by the
      current user only) and prints the public key as the bytes to put into
      uxplay_release_signature.cpp (release_key).  Once per project: a program
      built with another public key does not accept the releases signed with
      this one.

  sign_release.ps1 -KeyFile <key.pem> -Package <zip>
      writes <zip>.sig (the signature as 128 hexadecimal digits) beside the
      package, checks it, and prints the public key it belongs to.  Publish
      the .sig file as an asset of the release, next to the zip.

Needs PowerShell 7.2 or newer (.NET's ECDsa with PEM import and export).
#>
#requires -Version 7.2
param(
    [Parameter(Mandatory)][string]$KeyFile,
    [string]$Package,
    [switch]$NewKey
)
$ErrorActionPreference = 'Stop'
$sha256 = [System.Security.Cryptography.HashAlgorithmName]::SHA256

# the public key as the 64 bytes the program carries: X then Y
function Get-PublicPoint([System.Security.Cryptography.ECDsa]$Key) {
    $point = $Key.ExportParameters($false).Q
    return [byte[]]($point.X + $point.Y)
}

function Write-PublicPoint([byte[]]$Point) {
    Write-Output 'public key (X, Y), for release_key in uxplay_release_signature.cpp:'
    for ($row = 0; $row -lt 8; $row++) {
        $bytes = $Point[($row * 8)..($row * 8 + 7)] | ForEach-Object { '0x{0:x2}' -f $_ }
        Write-Output ('    ' + ($bytes -join ', ') + ',')
    }
}

if ($NewKey) {
    if (Test-Path -LiteralPath $KeyFile) {
        throw "the key file exists already and is not replaced: $KeyFile"
    }
    $folder = Split-Path -Parent ([System.IO.Path]::GetFullPath($KeyFile))
    New-Item -ItemType Directory -Force -Path $folder | Out-Null
    $key = [System.Security.Cryptography.ECDsa]::Create([System.Security.Cryptography.ECCurve+NamedCurves]::nistP256)
    [System.IO.File]::WriteAllText($KeyFile, $key.ExportPkcs8PrivateKeyPem() + "`n")
    # nobody but the user who made it reads it
    & icacls.exe $KeyFile /inheritance:r /grant:r "$([System.Security.Principal.WindowsIdentity]::GetCurrent().Name):(F)" | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "the key was written, but its permissions could not be restricted (icacls exit code $LASTEXITCODE): $KeyFile"
    }
    Write-Output "private key written to $KeyFile - keep a copy somewhere safe, and never publish it"
    Write-PublicPoint (Get-PublicPoint $key)
    return
}

if (-not $Package) {
    throw 'name the package to sign with -Package, or make a key with -NewKey'
}
$key = [System.Security.Cryptography.ECDsa]::Create()
$key.ImportFromPem([System.IO.File]::ReadAllText($KeyFile))
if ($key.KeySize -ne 256) {
    throw "the key in $KeyFile is not a P-256 key (it has $($key.KeySize) bits)"
}
$bytes = [System.IO.File]::ReadAllBytes($Package)
# IEEE P1363 form: r then s, 32 bytes each - what Windows' BCryptVerifySignature takes
$signature = $key.SignData($bytes, $sha256)
if ($signature.Length -ne 64 -or -not $key.VerifyData($bytes, $signature, $sha256)) {
    throw 'the signature that was just made does not verify'
}
$target = "$Package.sig"
[System.IO.File]::WriteAllText($target, [System.Convert]::ToHexString($signature).ToLowerInvariant() + "`n")
$digest = [System.Convert]::ToHexString([System.Security.Cryptography.SHA256]::HashData($bytes)).ToLowerInvariant()
Write-Output "signed $Package ($($bytes.Length) bytes, sha256 $digest)"
Write-Output "signature written to $target"
Write-PublicPoint (Get-PublicPoint $key)
