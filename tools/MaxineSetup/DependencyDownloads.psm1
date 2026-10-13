#requires -Version 5.1
Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

function Read-MPCVRDependencyLock {
    param([Parameter(Mandatory=$true)][string]$Path)
    $lock = Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
    if ($lock.schemaVersion -ne 1 -or $lock.repository -ne 'marcmy/VideoRenderer-Dependencies') { throw 'Unsupported dependency lock.' }
    foreach ($component in @('rife-runtime','rife-models','maxine')) {
        $pin = $lock.components.PSObject.Properties[$component].Value
        if (-not $pin -or $pin.manifestSha256 -notmatch '^[a-f0-9]{64}$') { throw "Missing dependency pin: $component" }
        $manifest = $pin.manifest
        $tag = [string]$pin.releaseTag
        if ($tag -notmatch '^mpcvr-deps-[a-z-]+-[a-f0-9]{16}$' -or $manifest.releaseTag -ne $tag -or $manifest.component -ne $component) { throw 'Invalid immutable dependency identity.' }
        $prefix = "https://github.com/marcmy/VideoRenderer-Dependencies/releases/download/$tag/"
        if ($pin.manifestUrl -cne ($prefix+$component+'.json')) { throw 'Dependency manifest URL is not pinned.' }
        foreach ($file in $manifest.files.PSObject.Properties) {
            $name = $file.Name; $entry = $file.Value
            $assetPrefix='https://github.com/marcmy/VideoRenderer-Dependencies/releases/download/'
            $assetPattern='^'+[regex]::Escape($assetPrefix)+'mpcvr-deps-(rife-runtime|rife-models|maxine)-[a-f0-9]{16}/'+[regex]::Escape($name)+'$'
            if ($name -notmatch '^[A-Za-z0-9][A-Za-z0-9._-]*$' -or $entry.sha256 -notmatch '^[a-f0-9]{64}$' -or [long]$entry.bytes -le 0 -or $entry.url -cnotmatch $assetPattern) { throw "Invalid pinned dependency: $name" }
        }
        $required = switch ($component) {
            'rife-runtime' { @('MPCVR-RIFE-Common.zip','runtime-manifest.json','SHA256SUMS.txt','MPCVR-RIFE-sm75.zip','MPCVR-RIFE-sm86.zip','MPCVR-RIFE-sm89.zip','MPCVR-RIFE-sm120.zip') }
            'rife-models' { @('MPCVR-RIFE-Models.zip','RIFE-model-license.txt','model-manifest.json') }
            'maxine' { @('MPCVR-Maxine-Runtime.zip','Maxine-SDK-License.pdf') }
        }
        foreach ($name in $required) { if (-not $manifest.files.PSObject.Properties[$name]) { throw "Incomplete $component pin: $name" } }
    }
    return $lock
}

function Get-MPCVRVerifiedDownload {
    param([Parameter(Mandatory=$true)]$Entry,[Parameter(Mandatory=$true)][string]$CacheRoot)
    $cache = Join-Path $CacheRoot $Entry.sha256
    if ((Test-Path -LiteralPath $cache -PathType Leaf) -and (Get-Item -LiteralPath $cache).Length -eq [long]$Entry.bytes -and (Get-FileHash -LiteralPath $cache -Algorithm SHA256).Hash -ieq $Entry.sha256) {
        Write-Host 'Reusing a verified dependency from the download cache.'
        return $cache
    }
    New-Item -ItemType Directory -Force -Path $CacheRoot | Out-Null
    $partial = $cache+'.'+[guid]::NewGuid().ToString('N')+'.partial'
    try {
        Write-Host ("Downloading {0:N1} MiB: {1}" -f ([long]$Entry.bytes/1MB),$Entry.url)
        $request=@{Uri=$Entry.url;OutFile=$partial}
        if ($PSVersionTable.PSEdition -eq 'Desktop') { $request.UseBasicParsing=$true }
        Invoke-WebRequest @request
        if ((Get-Item -LiteralPath $partial).Length -ne [long]$Entry.bytes -or (Get-FileHash -LiteralPath $partial -Algorithm SHA256).Hash -ine $Entry.sha256) { throw 'Dependency size or SHA-256 verification failed.' }
        # The cache is addressed by the expected hash; only verified data reaches it.
        Move-Item -LiteralPath $partial -Destination $cache -Force
        return $cache
    } finally {
        if (Test-Path -LiteralPath $partial) { Remove-Item -LiteralPath $partial -Force }
    }
}

function Resolve-MPCVRDependencies {
    param([Parameter(Mandatory=$true)]$Lock,[Parameter(Mandatory=$true)][string]$Destination,
          [Parameter(Mandatory=$true)][string[]]$ArchitectureKeys,
          [string]$CacheRoot=(Join-Path $env:LOCALAPPDATA 'MPCVideoRenderer\downloads'))
    foreach ($key in $ArchitectureKeys) { if ($key -notin @('sm75','sm86','sm89','sm120')) { throw "Unsupported GPU pack: $key" } }
    New-Item -ItemType Directory -Force -Path $Destination | Out-Null
    foreach ($component in @('rife-runtime','rife-models','maxine')) {
        $manifest=$Lock.components.PSObject.Properties[$component].Value.manifest
        foreach ($file in $manifest.files.PSObject.Properties) {
            $name=$file.Name
            if ($name -match '^MPCVR-RIFE-(sm\d+)\.zip$' -and $Matches[1] -notin $ArchitectureKeys) { continue }
            $cached=Get-MPCVRVerifiedDownload -Entry $file.Value -CacheRoot $CacheRoot
            $targetName=switch ($name) { 'runtime-manifest.json' {'RIFE-runtime-manifest.json'} 'SHA256SUMS.txt' {'RIFE-runtime-SHA256SUMS.txt'} default {$name} }
            Copy-Item -LiteralPath $cached -Destination (Join-Path $Destination $targetName)
        }
    }
    $tag=$Lock.components.PSObject.Properties['rife-runtime'].Value.releaseTag
    @{schemaVersion=1;releaseTag=$tag;assetBaseUrl="https://github.com/marcmy/VideoRenderer-Dependencies/releases/download/$tag"} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $Destination 'RIFE-runtime-release.json') -Encoding ASCII
    # Rebuild the staging list after every remote file has passed its pinned hash.
    $sums=@(Get-ChildItem -LiteralPath $Destination -Recurse -File | Where-Object {$_.Name -ne 'PAYLOAD-SHA256SUMS.txt'} | Sort-Object FullName | ForEach-Object {
        $relative=$_.FullName.Substring([IO.Path]::GetFullPath($Destination).TrimEnd('\').Length+1).Replace('\','/')
        '{0}  {1}' -f (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant(),$relative
    })
    $sums | Set-Content -LiteralPath (Join-Path $Destination 'PAYLOAD-SHA256SUMS.txt') -Encoding ASCII
}
Export-ModuleMember -Function Read-MPCVRDependencyLock,Get-MPCVRVerifiedDownload,Resolve-MPCVRDependencies
