#requires -Version 5.1

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

function Initialize-MpcvrTelemetryReader {
    if ('MpcvrUnifiedSetup.TelemetryReader' -as [type]) {
        return
    }

    Add-Type -TypeDefinition @'
using System;
using System.IO.MemoryMappedFiles;
using System.Runtime.InteropServices;
using System.Threading;

namespace MpcvrUnifiedSetup
{
    public static class TelemetryReader
    {
        public const int DataSize = 192;
        public const UInt32 Magic = 0x5256504d;
        public const UInt32 Version = 1;

        [DllImport("kernel32.dll")]
        private static extern UInt64 GetTickCount64();

        public static UInt64 CurrentTickMilliseconds()
        {
            return GetTickCount64();
        }

        public static byte[] Read(UInt32 processId)
        {
            return ReadMapping("Local\\MPCVR.UnifiedSetup.Telemetry." + processId.ToString());
        }

        private static byte[] ReadMapping(string mappingName)
        {
            try
            {
                using (MemoryMappedFile mapping = MemoryMappedFile.OpenExisting(
                    mappingName,
                    MemoryMappedFileRights.Read))
                using (MemoryMappedViewAccessor view = mapping.CreateViewAccessor(
                    0,
                    DataSize,
                    MemoryMappedFileAccess.Read))
                {
                    byte[] buffer = new byte[DataSize];
                    for (int attempt = 0; attempt < 50; attempt++)
                    {
                        int before = view.ReadInt32(12);
                        if ((before & 1) != 0)
                        {
                            Thread.SpinWait(64);
                            continue;
                        }

                        view.ReadArray(0, buffer, 0, buffer.Length);
                        int after = view.ReadInt32(12);
                        if (before == after && (after & 1) == 0)
                        {
                            if (BitConverter.ToUInt32(buffer, 0) != Magic ||
                                BitConverter.ToUInt32(buffer, 4) != Version ||
                                BitConverter.ToUInt32(buffer, 8) != DataSize)
                            {
                                return null;
                            }
                            return buffer;
                        }
                    }
                }
            }
            catch (System.IO.FileNotFoundException)
            {
                return null;
            }
            catch (UnauthorizedAccessException)
            {
                return null;
            }
            return null;
        }

        public static bool SelfTest()
        {
            string name = "Local\\MPCVR.UnifiedSetup.Telemetry.SelfTest." + Guid.NewGuid().ToString("N");
            using (MemoryMappedFile mapping = MemoryMappedFile.CreateNew(name, DataSize))
            using (MemoryMappedViewAccessor view = mapping.CreateViewAccessor())
            {
                view.Write(0, Magic);
                view.Write(4, Version);
                view.Write(8, (UInt32)DataSize);
                view.Write(12, 2);
                view.Write(16, (UInt32)1234);
                view.Write(20, (UInt32)0x3f);
                view.Write(24, (UInt64)987654321);
                view.Write(72, 59.94);
                view.Write(80, 119.88);
                view.Flush();

                byte[] result = ReadMapping(name);
                return result != null &&
                    BitConverter.ToUInt32(result, 16) == 1234 &&
                    Math.Abs(BitConverter.ToDouble(result, 72) - 59.94) < 0.0001 &&
                    Math.Abs(BitConverter.ToDouble(result, 80) - 119.88) < 0.0001;
            }
        }
    }
}
'@
}

function ConvertFrom-MpcvrTelemetryBytes {
    param(
        [Parameter(Mandatory)]
        [byte[]]$Bytes
    )

    $flags = [BitConverter]::ToUInt32($Bytes, 20)
    $updatedTick = [BitConverter]::ToUInt64($Bytes, 24)
    $currentTick = [MpcvrUnifiedSetup.TelemetryReader]::CurrentTickMilliseconds()
    $age = if ($currentTick -ge $updatedTick) { $currentTick - $updatedTick } else { 0 }

    return [pscustomobject]@{
        SchemaVersion = [BitConverter]::ToUInt32($Bytes, 4)
        ProcessId = [BitConverter]::ToUInt32($Bytes, 16)
        UpdatedTickMilliseconds = $updatedTick
        AgeMilliseconds = $age
        Fresh = $age -le 2000
        Flags = $flags
        RendererActive = ($flags -band 0x01) -ne 0
        MaxineEnabled = ($flags -band 0x02) -ne 0
        MaxineActive = ($flags -band 0x04) -ne 0
        FrameInterpolationEnabled = ($flags -band 0x08) -ne 0
        FrameInterpolationActive = ($flags -band 0x10) -ne 0
        CombinedActive = ($flags -band 0x20) -ne 0
        SourceWidth = [BitConverter]::ToUInt32($Bytes, 32)
        SourceHeight = [BitConverter]::ToUInt32($Bytes, 36)
        OutputWidth = [BitConverter]::ToUInt32($Bytes, 40)
        OutputHeight = [BitConverter]::ToUInt32($Bytes, 44)
        Frames = [BitConverter]::ToUInt64($Bytes, 48)
        DroppedFrames = [BitConverter]::ToUInt32($Bytes, 56)
        SkippedFrames = [BitConverter]::ToUInt32($Bytes, 60)
        FailedFrames = [BitConverter]::ToUInt32($Bytes, 64)
        SourceFps = [BitConverter]::ToDouble($Bytes, 72)
        TargetOutputFps = [BitConverter]::ToDouble($Bytes, 80)
        MeasuredDrawFps = [BitConverter]::ToDouble($Bytes, 88)
        MaxineVsrMilliseconds = [BitConverter]::ToDouble($Bytes, 96)
        MaxineDenoiseMilliseconds = [BitConverter]::ToDouble($Bytes, 104)
        MaxineDeblurMilliseconds = [BitConverter]::ToDouble($Bytes, 112)
        MaxineTotalMilliseconds = [BitConverter]::ToDouble($Bytes, 120)
        FrameInterpolationMilliseconds = [BitConverter]::ToDouble($Bytes, 128)
        CombinedProcessingMilliseconds = [BitConverter]::ToDouble($Bytes, 136)
        SourceFrameBudgetMilliseconds = [BitConverter]::ToDouble($Bytes, 144)
        TimingHeadroomMilliseconds = [BitConverter]::ToDouble($Bytes, 152)
        MaxineOperation = [BitConverter]::ToInt32($Bytes, 160)
        MaxineQuality = [BitConverter]::ToInt32($Bytes, 164)
        MaxineScale = [BitConverter]::ToInt32($Bytes, 168)
        MaxineOversample = [BitConverter]::ToInt32($Bytes, 172)
        FrameInterpolationMode = [BitConverter]::ToInt32($Bytes, 176)
        FrameInterpolationSourceLimit = [BitConverter]::ToInt32($Bytes, 180)
        FrameInterpolationMaxOutput = [BitConverter]::ToInt32($Bytes, 184)
    }
}

function Test-MpcvrTelemetryReader {
    Initialize-MpcvrTelemetryReader
    return [MpcvrUnifiedSetup.TelemetryReader]::SelfTest()
}

function Get-MpcvrRendererTelemetry {
    param(
        [uint32]$ProcessId,
        [ValidateRange(0, 300)]
        [int]$WaitSeconds = 0,
        [ValidateRange(10, 5000)]
        [int]$PollIntervalMilliseconds = 100
    )

    Initialize-MpcvrTelemetryReader
    $deadline = [DateTime]::UtcNow.AddSeconds($WaitSeconds)
    do {
        $candidateProcessIds = @()
        if ($ProcessId -gt 0) {
            $candidateProcessIds = @($ProcessId)
        }
        else {
            $candidateProcessIds = @(Get-Process -Name 'mpc-hc', 'mpc-hc64' -ErrorAction SilentlyContinue |
                Select-Object -ExpandProperty Id)
        }

        $results = @()
        foreach ($candidateId in $candidateProcessIds) {
            $bytes = [MpcvrUnifiedSetup.TelemetryReader]::Read([uint32]$candidateId)
            if ($null -ne $bytes) {
                $results += ConvertFrom-MpcvrTelemetryBytes -Bytes $bytes
            }
        }

        if ($results.Count -gt 0 -or [DateTime]::UtcNow -ge $deadline) {
            return $results
        }
        Start-Sleep -Milliseconds $PollIntervalMilliseconds
    } while ($true)
}

Export-ModuleMember -Function @(
    'Initialize-MpcvrTelemetryReader',
    'ConvertFrom-MpcvrTelemetryBytes',
    'Test-MpcvrTelemetryReader',
    'Get-MpcvrRendererTelemetry'
)
