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
          [AllowEmptyCollection()][ValidateSet('rife-runtime','rife-models','maxine')][string[]]$Components=@('rife-runtime','rife-models','maxine'),
          [string]$CacheRoot=(Join-Path $env:LOCALAPPDATA 'MPCVideoRenderer\downloads'))
    foreach ($key in $ArchitectureKeys) { if ($key -notin @('sm75','sm86','sm89','sm120')) { throw "Unsupported GPU pack: $key" } }
    New-Item -ItemType Directory -Force -Path $Destination | Out-Null
    foreach ($component in $Components) {
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

function Get-MPCVRInstalledDependencyInventory {
    param($Lock,[string]$Component,[string]$InstallRoot,[string[]]$ArchitectureKeys,
          [string]$CacheRoot=(Join-Path $env:LOCALAPPDATA 'MPCVideoRenderer\downloads'))
    $components=if ($Component -eq 'maxine') { @('maxine') } else { @('rife-runtime','rife-models') }
    $pins=@($components | ForEach-Object {
        $pin=$Lock.components.PSObject.Properties[$_].Value
        [ordered]@{component=$_;releaseTag=$pin.releaseTag;manifestSha256=$pin.manifestSha256}
    })
    $keys=if ($Component -eq 'rife') { @($ArchitectureKeys | Sort-Object -Unique) } else { @() }
    # Identity follows immutable dependencies and GPU packs, never renderer versions.
    $identity=ConvertTo-Json -InputObject ([ordered]@{pins=$pins;architectures=$keys}) -Depth 8 -Compress
    $receiptPath=Join-Path $InstallRoot 'dependency-receipt.json'
    if (Test-Path -LiteralPath $receiptPath -PathType Leaf) {
        $receipt=Get-Content -LiteralPath $receiptPath -Raw | ConvertFrom-Json
        if ($receipt.schemaVersion -ne 1 -or $receipt.component -ne $Component -or $receipt.identity -cne $identity) { return $null }
        if (@($receipt.files).Count -eq 0) { return $null }
        return $receipt
    }

    # Adopt older installs without rewriting them when their cached, pinned ZIPs
    # can prove the expected file hashes. No network or DLL loading here.
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $files=@{}
    $expectedModels=@()
    foreach ($componentName in $components) {
        $manifest=$Lock.components.PSObject.Properties[$componentName].Value.manifest
        $archives=switch ($componentName) {
            'maxine' { @('MPCVR-Maxine-Runtime.zip') }
            'rife-models' { @('MPCVR-RIFE-Models.zip') }
            'rife-runtime' { @('MPCVR-RIFE-Common.zip') + @($keys | ForEach-Object { "MPCVR-RIFE-$_.zip" }) }
        }
        foreach ($name in $archives) {
            $entry=$manifest.files.PSObject.Properties[$name].Value
            $archivePath=Join-Path $CacheRoot $entry.sha256
            if (-not (Test-Path -LiteralPath $archivePath -PathType Leaf) -or
                (Get-Item -LiteralPath $archivePath).Length -ne [long]$entry.bytes -or
                (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash -ine $entry.sha256) { return $null }
            $archive=[IO.Compression.ZipFile]::OpenRead($archivePath)
            try {
                if ($componentName -eq 'rife-models') {
                    $modelMetadata=$archive.GetEntry('model-manifest.json')
                    if (-not $modelMetadata) { return $null }
                    $reader=New-Object IO.StreamReader ($modelMetadata.Open())
                    try { $modelManifest=$reader.ReadToEnd() | ConvertFrom-Json } finally { $reader.Dispose() }
                    if ($modelManifest.schemaVersion -ne 3 -or @($modelManifest.models).Count -eq 0) { return $null }
                    $expectedModels=@($modelManifest.models)
                }
                $prefix=''
                if ($componentName -eq 'maxine') {
                    $candidates=@($archive.Entries | Where-Object { $_.Name -ieq 'NVCVImage.dll' })
                    if ($candidates.Count -ne 1) { return $null }
                    $prefix=$candidates[0].FullName.Replace('\','/') -replace 'NVCVImage\.dll$',''
                    foreach ($required in @('NVVideoEffects.dll','nvngxruntime.dll','nvngx_vsr.dll','nvVFXVideoSuperRes.dll')) {
                        if (-not @($archive.Entries | Where-Object { $_.FullName.Replace('\','/') -ieq ($prefix+$required) }).Count) { return $null }
                    }
                }
                $count=0
                foreach ($file in $archive.Entries) {
                    if (-not $file.Name) { continue }
                    $relative=$file.FullName.Replace('\','/')
                    if ($componentName -eq 'maxine' -and -not $relative.StartsWith($prefix,[StringComparison]::OrdinalIgnoreCase)) { continue }
                    if ($componentName -eq 'rife-models' -and $relative -notmatch '^[^/]+\.onnx$') { continue }
                    switch ($componentName) {
                        'maxine' {
                            $relative='nvvfx/libs/'+$relative.Substring($prefix.Length)
                        }
                        'rife-runtime' { $relative='runtime/'+$relative }
                        'rife-models' {
                            $relative='models/'+$relative
                        }
                    }
                    $stream=$file.Open(); $hasher=[Security.Cryptography.SHA256]::Create()
                    try { $hash=([BitConverter]::ToString($hasher.ComputeHash($stream))).Replace('-','').ToLowerInvariant() }
                    finally { $stream.Dispose(); $hasher.Dispose() }
                    if ($files.ContainsKey($relative) -and $files[$relative].sha256 -ne $hash) { return $null }
                    $files[$relative]=[pscustomobject]@{path=$relative;bytes=$file.Length;sha256=$hash}
                    $count++
                }
                if ($count -eq 0) { return $null }
            } finally { $archive.Dispose() }
        }
    }
    if ($Component -eq 'rife') {
        $installedManifest=Join-Path $InstallRoot 'installed-manifest.json'
        if (-not (Test-Path -LiteralPath $installedManifest -PathType Leaf)) { return $null }
        $metadata=Get-Content -LiteralPath $installedManifest -Raw | ConvertFrom-Json
        $runtimePin=$Lock.components.PSObject.Properties['rife-runtime'].Value.manifest
        if ($metadata.schemaVersion -ne 2 -or $metadata.runtimeAbi -ne $runtimePin.compatibility.runtimeAbi -or
            [string]$metadata.tensorRtVersion -notmatch '^\d+\.\d+(\.\d+){0,2}$' -or
            $metadata.commonRuntimeArchiveSha256 -ine $runtimePin.files.PSObject.Properties['MPCVR-RIFE-Common.zip'].Value.sha256) { return $null }
        foreach ($key in $keys) {
            $selected=@($metadata.architectures | Where-Object { $_.key -eq $key })
            $archiveName="MPCVR-RIFE-$key.zip"
            if ($selected.Count -ne 1 -or $selected[0].archive -ne $archiveName -or
                $selected[0].sha256 -ine $runtimePin.files.PSObject.Properties[$archiveName].Value.sha256 -or
                $selected[0].builderResource -ne "nvinfer_builder_resource_${key}_11.dll") { return $null }
        }
        if (@($metadata.models).Count -ne $expectedModels.Count) { return $null }
        foreach ($expected in $expectedModels) {
            $modelFile='models/'+$expected.file
            $selected=@($metadata.models | Where-Object { $_.model -eq $expected.model -and $_.modelFile -eq $modelFile })
            if ($selected.Count -ne 1 -or -not $files.ContainsKey($modelFile) -or $selected[0].modelSha256 -ine $files[$modelFile].sha256) { return $null }
        }
        $files['installed-manifest.json']=[pscustomobject]@{
            path='installed-manifest.json';bytes=(Get-Item -LiteralPath $installedManifest).Length
            sha256=(Get-FileHash -LiteralPath $installedManifest -Algorithm SHA256).Hash.ToLowerInvariant()
        }
    }
    return [pscustomobject]@{schemaVersion=1;component=$Component;identity=$identity;files=@($files.Values | Sort-Object path)}
}

function Test-MPCVRDependencyInventory {
    param($Inventory,[string]$InstallRoot)
    if (-not $Inventory) { return $false }
    $root=[IO.Path]::GetFullPath($InstallRoot).TrimEnd('\','/')+[IO.Path]::DirectorySeparatorChar
    foreach ($file in $Inventory.files) {
        # A malformed receipt can only force repair, never read outside this root.
        if ($file.path -match '(^[\\/]|:|(^|[\\/])\.\.([\\/]|$))' -or $file.sha256 -notmatch '^[a-f0-9]{64}$' -or [long]$file.bytes -lt 0) { return $false }
        $path=[IO.Path]::GetFullPath((Join-Path $InstallRoot $file.path))
        if (-not $path.StartsWith($root,[StringComparison]::OrdinalIgnoreCase) -or
            -not (Test-Path -LiteralPath $path -PathType Leaf) -or
            (Get-Item -LiteralPath $path).Length -ne [long]$file.bytes -or
            (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ine $file.sha256) { return $false }
    }
    return $true
}

function Test-MPCVRInstalledDependency {
    param([Parameter(Mandatory=$true)]$Lock,[Parameter(Mandatory=$true)][ValidateSet('maxine','rife')][string]$Component,
          [Parameter(Mandatory=$true)][string]$InstallRoot,[Parameter(Mandatory=$true)][string[]]$ArchitectureKeys,
          [string]$CacheRoot=(Join-Path $env:LOCALAPPDATA 'MPCVideoRenderer\downloads'))
    try {
        $inventory=Get-MPCVRInstalledDependencyInventory @PSBoundParameters
        return Test-MPCVRDependencyInventory -Inventory $inventory -InstallRoot $InstallRoot
    } catch { return $false } # Unreadable/malformed/missing state must never skip repair.
}

function Save-MPCVRInstalledDependency {
    param([Parameter(Mandatory=$true)]$Lock,[Parameter(Mandatory=$true)][ValidateSet('maxine','rife')][string]$Component,
          [Parameter(Mandatory=$true)][string]$InstallRoot,[Parameter(Mandatory=$true)][string[]]$ArchitectureKeys,
          [string]$CacheRoot=(Join-Path $env:LOCALAPPDATA 'MPCVideoRenderer\downloads'))
    # Called only after installer and preflight success. A newly installed root
    # has no receipt, so expected hashes come from the pinned archives.
    $receipt=Join-Path $InstallRoot 'dependency-receipt.json'
    $inventory=Get-MPCVRInstalledDependencyInventory @PSBoundParameters
    if (-not (Test-MPCVRDependencyInventory -Inventory $inventory -InstallRoot $InstallRoot)) { throw "Cannot record unverified $Component installation." }
    $temporary=$receipt+'.'+[guid]::NewGuid().ToString('N')+'.tmp'
    try {
        $inventory | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $temporary -Encoding UTF8
        Move-Item -LiteralPath $temporary -Destination $receipt -Force
    } finally { if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary -Force } }
}

Export-ModuleMember -Function Read-MPCVRDependencyLock,Get-MPCVRVerifiedDownload,Resolve-MPCVRDependencies,Test-MPCVRInstalledDependency,Save-MPCVRInstalledDependency
