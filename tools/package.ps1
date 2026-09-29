<#
    package.ps1 - stage and zip a release from a finished build (called by package.cmd).

    The zip holds one folder, DeckBtService-<version>\, with the programs, the packaging scripts and
    their launchers, LICENSE, and SHA256SUMS (lowercase SHA-256, two spaces, file name; not listing itself),
    which install.ps1 checks. A clean build is named after VERSION alone; a build with uncommitted
    changes is refused unless DECKBT_ALLOW_DIRTY=1, and is then named after its full identity.
#>
param(
    [Parameter(Mandatory)] [string] $Root,
    [Parameter(Mandatory)] [string] $BuildOut
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$Root = [IO.Path]::GetFullPath($Root)
$identity = (Get-Content -LiteralPath (Join-Path $BuildOut 'build-version.txt') -Raw).Trim()
if ($identity.EndsWith('-dirty')) {
    if ($env:DECKBT_ALLOW_DIRTY -ne '1') {
        throw "Refusing to package $identity`: the working tree has uncommitted changes. Commit them, or set DECKBT_ALLOW_DIRTY=1 for a test package."
    }
    $name = "DeckBtService-$identity"
} else {
    $name = 'DeckBtService-' + (Get-Content -LiteralPath (Join-Path $Root 'VERSION') -Raw).Trim()
}

$programs = @('deckbt-usbip.exe', 'deckbt-uartprobe.exe')
$packaging = @('install.cmd', 'uninstall.cmd', 'collect-diagnostics.cmd',
               'install.ps1', 'uninstall.ps1', 'collect-diagnostics.ps1', 'README-install.md')

$stage = Join-Path $BuildOut ('stage-' + [Guid]::NewGuid().ToString('N'))
$release = Join-Path $stage $name
New-Item -ItemType Directory -Path $release | Out-Null
try {
    foreach ($file in $programs) { Copy-Item -LiteralPath (Join-Path $BuildOut $file) -Destination $release }
    foreach ($file in $packaging) { Copy-Item -LiteralPath (Join-Path $Root "packaging\$file") -Destination $release }
    Copy-Item -LiteralPath (Join-Path $Root 'LICENSE') -Destination $release

    $sums = @(Get-ChildItem -LiteralPath $release -File | Sort-Object Name | ForEach-Object {
        (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() + '  ' + $_.Name
    })
    [IO.File]::WriteAllLines((Join-Path $release 'SHA256SUMS'), $sums, [Text.Encoding]::ASCII)

    $dist = Join-Path $Root 'dist'
    New-Item -ItemType Directory -Path $dist -Force | Out-Null
    $zip = Join-Path $dist "$name.zip"
    $temporary = "$zip.$([Guid]::NewGuid().ToString('N')).tmp"
    Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
    try {
        # Entries added one by one: ZipFile.CreateFromDirectory on .NET Framework writes '\' separators.
        $archive = [IO.Compression.ZipFile]::Open($temporary, [IO.Compression.ZipArchiveMode]::Create)
        try {
            foreach ($file in @(Get-ChildItem -LiteralPath $release -File | Sort-Object Name)) {
                [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
                    $archive, $file.FullName, "$name/$($file.Name)", [IO.Compression.CompressionLevel]::Optimal)
            }
        } finally {
            $archive.Dispose()
        }
        Move-Item -LiteralPath $temporary -Destination $zip -Force
    } finally {
        if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary -Force }
    }
    Write-Host "Packaged $zip"
} finally {
    Remove-Item -LiteralPath $stage -Recurse -Force
}
