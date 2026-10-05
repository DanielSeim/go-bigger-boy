param(
    [Parameter(Mandatory=$true)][string]$Executable,
    [Parameter(Mandatory=$true)][string]$Rom,
    [Parameter(Mandatory=$true)][string]$FirmwareDirectory,
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [string]$InitialSave,
    [ValidateRange(1,36000)][int]$Frames=7200,
    [switch]$Lifecycle,
    [switch]$BundledBootstrap
)
# Caller-owned inputs only. A fresh output directory isolates preferences and
# battery saves; neither installed settings nor original saves are modified.
$ErrorActionPreference='Stop'
if($Lifecycle -and $Frames -lt 1800) { throw 'Lifecycle checks require at least 1800 frames' }
if($Lifecycle) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class GbbPlaybackWindow {
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr hwnd, uint msg, UIntPtr command, IntPtr data);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
    [DllImport("user32.dll")] public static extern IntPtr GetMenu(IntPtr hwnd);
    public static bool Command(IntPtr hwnd, uint command) { return PostMessage(hwnd, 0x111, new UIntPtr(command), IntPtr.Zero); }
}
'@
}
foreach ($path in @($Executable,$Rom,$FirmwareDirectory,$OutputDirectory)) {
    if ($path.Contains('"')) { throw 'Paths containing quotes are not supported' }
}
$Executable=(Resolve-Path -LiteralPath $Executable).ProviderPath
$Rom=(Resolve-Path -LiteralPath $Rom).ProviderPath
$FirmwareDirectory=(Resolve-Path -LiteralPath $FirmwareDirectory).ProviderPath
if (Test-Path -LiteralPath $OutputDirectory) { throw 'Use a new output directory' }
New-Item -ItemType Directory -Path $OutputDirectory | Out-Null
$OutputDirectory=(Resolve-Path -LiteralPath $OutputDirectory).ProviderPath
$savedEnvironment=@{}
foreach($name in @('GBB_FRAME_TIMING','GBB_FRAME_TIMING_FILE','GBB_FRONTEND_TEST_DIRECTORY','SDL_AUDIODRIVER')) {
    $savedEnvironment[$name]=[Environment]::GetEnvironmentVariable($name,'Process')
}
try {
    $env:GBB_FRAME_TIMING='1'
    $env:SDL_AUDIODRIVER='wasapi'
    $hashes=@{}
    if($BundledBootstrap) {
        $bundledDirectory=Join-Path $OutputDirectory 'firmware-bundled'
        New-Item -ItemType Directory -Path $bundledDirectory | Out-Null
        foreach($name in @('sgb1.program.rom','sgb2.program.rom','spc700.rom')) {
            Copy-Item -LiteralPath (Join-Path $FirmwareDirectory $name) -Destination (Join-Path $bundledDirectory $name)
        }
        $FirmwareDirectory=$bundledDirectory
    }
    foreach($name in @('sgb1.program.rom','sgb2.program.rom','spc700.rom')) {
        $hashes[$name]=(Get-FileHash -LiteralPath (Join-Path $FirmwareDirectory $name) -Algorithm SHA256).Hash
    }
    $bootModes=@{}
    foreach($name in @('sgb.boot.rom','sgb2.boot.rom')) {
        $path=Join-Path $FirmwareDirectory $name
        if(Test-Path -LiteralPath $path) {
            $hashes[$name]=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
            $bootModes[$name]='external override'
        } else { $bootModes[$name]='bundled' }
    }
    $metadata=@{executable_sha256=(Get-FileHash -LiteralPath $Executable -Algorithm SHA256).Hash;
        rom_sha256=(Get-FileHash -LiteralPath $Rom -Algorithm SHA256).Hash;
        firmware_sha256=$hashes; gb_boot_modes=$bootModes; frames=$Frames; utc_started=[DateTime]::UtcNow.ToString('o');
        power_profile=(& powercfg.exe /getactivescheme | Out-String).Trim();
        audio_backend='wasapi'; lifecycle=[bool]$Lifecycle; physical_listening='not recorded; report separately'}
    if ($InitialSave) { $metadata.initial_save_sha256=(Get-FileHash -LiteralPath $InitialSave -Algorithm SHA256).Hash }
    $metadata | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'provenance.json')
    foreach($model in @('sgb','sgb2')) {
        $slot=Join-Path $OutputDirectory $model
        New-Item -ItemType Directory -Path $slot | Out-Null
        $game=Join-Path $slot 'game.gb'
        Copy-Item -LiteralPath $Rom -Destination $game
        if($InitialSave) { Copy-Item -LiteralPath $InitialSave -Destination (Join-Path $slot "game.$model-firmware.sav") }
        $env:GBB_FRONTEND_TEST_DIRECTORY=Join-Path $slot 'preferences'
        $env:GBB_FRAME_TIMING_FILE=Join-Path $slot 'frame-timing.log'
        $arguments='"{0}" --sgb-firmware "{1}" --sgb-model {2} --frontend-smoke-frames {3}' -f $game,$FirmwareDirectory,$model,$Frames
        Write-Output "Starting $model real-window/real-audio playback; please listen. Frames=$Frames"
        $process=Start-Process -FilePath $Executable -ArgumentList $arguments -WorkingDirectory $slot -PassThru `
            -RedirectStandardOutput (Join-Path $slot 'stdout.log') -RedirectStandardError (Join-Path $slot 'stderr.log')
        # Windows PowerShell may otherwise release the process handle before
        # ExitCode is queried after an asynchronous GUI process exits.
        $null=$process.Handle
        if($Lifecycle) {
            $deadline=[DateTime]::UtcNow.AddSeconds(30)
            do {
                if($process.HasExited) { throw 'Playback exited before lifecycle checks' }
                $process.Refresh()
                $window=$process.MainWindowHandle
                if($window -ne [IntPtr]::Zero -and [GbbPlaybackWindow]::GetMenu($window) -ne [IntPtr]::Zero) { break }
                Start-Sleep -Milliseconds 100
            } while([DateTime]::UtcNow -lt $deadline)
            if($window -eq [IntPtr]::Zero -or [GbbPlaybackWindow]::GetMenu($window) -eq [IntPtr]::Zero) { throw 'No native GBB menu window' }
            $sendCommand={ param([int]$command)
                [uint32]$owner=0
                [void][GbbPlaybackWindow]::GetWindowThreadProcessId($window,[ref]$owner)
                if($owner -ne $process.Id -or $process.HasExited) { throw 'Test window no longer belongs to test process' }
                if(-not [GbbPlaybackWindow]::Command($window,[uint32]$command)) { throw 'Menu command failed' }
                "command=$command utc=$([DateTime]::UtcNow.ToString('o'))" | Add-Content -LiteralPath (Join-Path $slot 'lifecycle.log')
            }
            Start-Sleep -Seconds 5
            & $sendCommand 0x8105 # Pause; only the test HWND receives input.
            Start-Sleep -Seconds 3
            if(-not (Select-String -LiteralPath $env:GBB_FRAME_TIMING_FILE -Pattern ' core_steps=0 ' -Quiet)) { throw 'No paused timing window' }
            & $sendCommand 0x8105 # Resume.
            Start-Sleep -Seconds 3
            & $sendCommand 0x8102 # Save isolated manual snapshot.
            Start-Sleep -Seconds 2
            $states=@(Get-ChildItem -LiteralPath $env:GBB_FRONTEND_TEST_DIRECTORY -Recurse -Filter '*.gbbs')
            if($states.Count -ne 1 -or $states[0].Name -notlike "*.$model-firmware.gbbs") { throw 'Missing or nonisolated firmware snapshot' }
            $stream=[IO.File]::OpenRead($states[0].FullName)
            try {
                $magic=New-Object byte[] 8
                if($stream.Read($magic,0,8) -ne 8 -or [Text.Encoding]::ASCII.GetString($magic) -ne 'GBBFW001') { throw 'Wrong snapshot format' }
            } finally { $stream.Dispose() }
            & $sendCommand 0x8106 # Cold reset.
            Start-Sleep -Seconds 3
            & $sendCommand 0x8103 # Restore snapshot through the native menu.
            Start-Sleep -Seconds 3
        }
        if(-not $process.WaitForExit([int](($Frames/50+120)*1000))) {
            throw "Test process $($process.Id) did not exit within its bounded deadline; inspect its window"
        }
        $process.WaitForExit()
        $process.Refresh()
        if($process.ExitCode -ne 0) { throw "$model playback failed: exit $($process.ExitCode); see $slot" }
        if(-not (Test-Path -LiteralPath $env:GBB_FRAME_TIMING_FILE)) { throw "$model produced no timing trace" }
        if($Lifecycle) {
            if(-not (Select-String -LiteralPath $env:GBB_FRAME_TIMING_FILE -Pattern '^firmware_qualification_complete ' -Quiet)) { throw 'Lifecycle run did not finish playback' }
            Write-Output "$model native menu pause/resume, isolated save, reset and load smoke PASS (not a steady-state FPS run)"
        }
    }
    Write-Output "Completed. Check both frame-timing.log files with scripts/check_sgb_frontend_playback.py."
    Write-Output 'These results do not assert audible fidelity. Lifecycle traces must not qualify steady-state FPS.'
} finally {
    if($process -and -not $process.HasExited) {
        [void]$process.CloseMainWindow()
        if(-not $process.WaitForExit(10000)) { Write-Warning "Test process $($process.Id) is still open; inspect its window" }
    }
    foreach($entry in $savedEnvironment.GetEnumerator()) {
        [Environment]::SetEnvironmentVariable($entry.Key,$entry.Value,'Process')
    }
}
