Set-StrictMode -Version 2.0

function Install-KLiteRendererFiles {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [hashtable]$Sources,
        [Parameter(Mandatory = $true)]
        [hashtable]$Targets
    )

    if ($Targets.Count -eq 0) {
        throw 'No renderer targets were supplied.'
    }

    $backups = @()
    $installed = @()

    try {
        foreach ($fileName in @($Targets.Keys | Sort-Object)) {
            if (-not $Sources.ContainsKey($fileName)) {
                throw "No renderer source was supplied for $fileName."
            }

            $source = (Resolve-Path -LiteralPath ([string]$Sources[$fileName])).Path
            $destination = [IO.Path]::GetFullPath([string]$Targets[$fileName])
            if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
                throw "Renderer source was not found: $source"
            }
            if (-not (Test-Path -LiteralPath $destination -PathType Leaf)) {
                throw "Existing renderer target was not found: $destination"
            }

            $backup = "$destination.mpcvr-backup-$([guid]::NewGuid().ToString('N'))"
            Copy-Item -LiteralPath $destination -Destination $backup -Force
            $backups += [pscustomobject]@{
                File = $fileName
                Destination = $destination
                Backup = $backup
                OriginalSha256 = (Get-FileHash -LiteralPath $backup -Algorithm SHA256).Hash.ToLowerInvariant()
            }

            Copy-Item -LiteralPath $source -Destination $destination -Force
            $sourceHash = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToLowerInvariant()
            $destinationHash = (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash.ToLowerInvariant()
            if ($sourceHash -ne $destinationHash) {
                throw "Verification failed after copying $fileName."
            }

            $item = Get-Item -LiteralPath $destination
            $installed += [pscustomobject]@{
                File = $item.Name
                Version = $item.VersionInfo.FileVersion
                Path = $item.FullName
                Sha256 = $destinationHash
            }
        }

        foreach ($entry in $backups) {
            Remove-Item -LiteralPath $entry.Backup -Force -ErrorAction SilentlyContinue
        }
        $backups = @()

        return $installed
    }
    catch {
        $installError = $_
        $rollbackErrors = @()

        for ($index = $backups.Count - 1; $index -ge 0; $index--) {
            $entry = $backups[$index]
            try {
                Copy-Item -LiteralPath $entry.Backup -Destination $entry.Destination -Force
                $restoredHash = (Get-FileHash -LiteralPath $entry.Destination -Algorithm SHA256).Hash.ToLowerInvariant()
                if ($restoredHash -ne [string]$entry.OriginalSha256) {
                    throw "Restored hash for $($entry.File) does not match the original file."
                }
            }
            catch {
                $rollbackErrors += "$($entry.File): $($_.Exception.Message)"
            }
        }

        if ($rollbackErrors.Count -ne 0) {
            throw "Renderer installation failed: $($installError.Exception.Message) Rollback also failed: $($rollbackErrors -join '; ')"
        }
        throw $installError
    }
    finally {
        foreach ($entry in $backups) {
            Remove-Item -LiteralPath $entry.Backup -Force -ErrorAction SilentlyContinue
        }
    }
}

Export-ModuleMember -Function 'Install-KLiteRendererFiles'
