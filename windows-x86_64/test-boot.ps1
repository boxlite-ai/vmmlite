param(
    [string]$Kernel = "$PSScriptRoot\bzImage",
    [string]$Initramfs = "$PSScriptRoot\initramfs.cpio",
    [ValidateRange(1, 3600)][int]$TimeoutSeconds = 120
)
$ErrorActionPreference = 'Stop'
$binary = (Resolve-Path "$PSScriptRoot\vmmlite.exe").Path
$kernelPath = (Resolve-Path $Kernel).Path
$initramfsPath = (Resolve-Path $Initramfs).Path
& $binary --probe
if ($LASTEXITCODE -ne 0) { throw 'WHP capability probe failed.' }
$stdout = "$PSScriptRoot\boot.stdout.log"
$stderr = "$PSScriptRoot\boot.stderr.log"
# Windows filenames cannot contain a double quote; quote complete resolved file paths.
$start = New-Object System.Diagnostics.ProcessStartInfo
$start.FileName = $binary
$start.Arguments = '"' + $kernelPath + '" "' + $initramfsPath + '"'
$start.UseShellExecute = $false
$start.RedirectStandardOutput = $true
$start.RedirectStandardError = $true
$process = New-Object System.Diagnostics.Process
$process.StartInfo = $start
[void]$process.Start()
# Drain both pipes concurrently so a verbose kernel cannot fill either pipe and stall.
$stdoutRead = $process.StandardOutput.ReadToEndAsync()
$stderrRead = $process.StandardError.ReadToEndAsync()
$finished = $process.WaitForExit($TimeoutSeconds * 1000)
if (-not $finished) { $process.Kill() }
$process.WaitForExit()
$guestLog = $stdoutRead.GetAwaiter().GetResult()
$hostLog = $stderrRead.GetAwaiter().GetResult()
[System.IO.File]::WriteAllText($stdout, $guestLog)
[System.IO.File]::WriteAllText($stderr, $hostLog)
$exitCode = $process.ExitCode
$process.Dispose()
if (-not $finished) { throw "Guest exceeded $TimeoutSeconds seconds. See both boot logs." }
if ($exitCode -ne 0 -or $guestLog -notmatch 'Hello from PID 1!' -or
    $hostLog -notmatch 'guest reset; virtio TX bytes=[1-9][0-9]*') {
    throw "Guest boot not established (exit $exitCode). See both boot logs."
}
[ordered]@{
    utc = [DateTime]::UtcNow.ToString('o')
    windows = [Environment]::OSVersion.VersionString
    exitCode = $exitCode
    executableSha256 = (Get-FileHash $binary -Algorithm SHA256).Hash
    kernelSha256 = (Get-FileHash $kernelPath -Algorithm SHA256).Hash
    initramfsSha256 = (Get-FileHash $initramfsPath -Algorithm SHA256).Hash
} | ConvertTo-Json | Set-Content "$PSScriptRoot\boot.evidence.json"
Write-Output 'WHP guest printed the PID 1 message, used the virtio console, and reset.'
