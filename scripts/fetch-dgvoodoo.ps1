$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$cache = Join-Path $repo '.cache\dgVoodoo-2.87.5'
$archive = Join-Path $cache 'dgVoodoo2_87_5.zip'
$expected = '5FFDE6927F7355CA3FDD5D785B581256A8E6539FA13E395A891ADE6BA1040850'
New-Item -ItemType Directory -Force $cache | Out-Null
if (!(Test-Path -LiteralPath $archive)) {
    Invoke-WebRequest 'https://github.com/dege-diosg/dgVoodoo2/releases/download/v2.87.5/dgVoodoo2_87_5.zip' -OutFile $archive
}
if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $expected) {
    throw "Unexpected dgVoodoo archive hash. Inspect $archive before continuing."
}
$expanded = Join-Path $cache 'expanded'
Expand-Archive -LiteralPath $archive -DestinationPath $expanded -Force
$vendor = Join-Path $repo 'vendor\dgVoodoo'
New-Item -ItemType Directory -Force $vendor | Out-Null
foreach ($name in @('DDraw.dll', 'D3DImm.dll')) {
    Copy-Item -LiteralPath (Join-Path $expanded "MS\x86\$name") -Destination $vendor -Force
}
Write-Output 'Verified dgVoodoo 2.87.5 x86 dependencies are ready.'
