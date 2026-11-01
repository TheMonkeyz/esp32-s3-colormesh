# Reads the board's serial output into <repo>\.devloop (docs/PROTOCOL.md section 1). Started by flash_helper.ps1.
# Stops when the time is up, when .devloop\stop.request appears or Q / Esc is pressed in this window (exit code 2).
# While it runs (for tools/harness):
#   serial_live.txt  grows line by line (the whole log is also written to serial_log.txt at the end)
#   serial.send      each line in it is sent to the board's test console (forge_core/testcon), then the file is
#                    deleted; the sent line also appears in the logs as "> command". Writers write serial.send.tmp
#                    and rename it, so a half-written file is never read.
# -Reboot: restart the board first with the test console's `reboot`, on this open port. A software restart keeps the
# S3's USB connected, so the boot log is whole from ESP-ROM on; esptool's hard reset re-enumerates the USB and the first
# ~2.5 s are lost (the flash mode, the reset reason: LESSONS L154, L191). Exit code 3 when no boot follows within 4 s
# (firmware without the console, a hung board): the helper then resets with esptool and runs this again.
# Baud from forge.json (monitor_baud); port: -Port, else forge.json's port, else the last one esptool used.
param([string]$Port = "", [int]$Seconds = 75, [int]$Baud = 0, [switch]$Reboot)

function Find-Root {
  $d = $PSScriptRoot
  while ($d) {
    if (Test-Path (Join-Path $d "forge.json")) { return $d }
    $up = Split-Path $d -Parent
    if (-not $up -or $up -eq $d) { break }
    $d = $up
  }
  throw "no forge.json above $PSScriptRoot"
}
$Root = Find-Root
$Cfg = Get-Content (Join-Path $Root "forge.json") -Raw | ConvertFrom-Json
$Dev = Join-Path $Root ".devloop"
New-Item -ItemType Directory -Force $Dev | Out-Null
Set-Location $Dev

if (-not $Baud) { $Baud = if ($Cfg.monitor_baud) { [int]$Cfg.monitor_baud } else { 115200 } }
if (-not $Port -and $Cfg.port) { $Port = $Cfg.port }
if (-not $Port -and (Test-Path flash_log.txt)) {
  $m = Select-String -Path flash_log.txt -Pattern 'Serial port (COM\d+)' | Select-Object -Last 1
  if ($m) { $Port = $m.Matches[0].Groups[1].Value }
}
if (-not $Port) { $Port = [System.IO.Ports.SerialPort]::GetPortNames() | Select-Object -Last 1 }
Write-Host "Monitoring $Port at $Baud baud ..."
if (-not $Reboot) { Start-Sleep -Seconds 2 }   # let USB re-enumerate after reset
$sp = New-Object System.IO.Ports.SerialPort $Port, $Baud
$sp.DtrEnable = $false; $sp.RtsEnable = $false; $sp.ReadTimeout = 200
$out = New-Object System.Collections.Generic.List[string]
for ($i = 0; $i -lt 5 -and -not $sp.IsOpen; $i++) { try { $sp.Open() } catch { Start-Sleep -Seconds 1 } }
if (-not $sp.IsOpen) { "Could not open $Port" | Tee-Object serial_log.txt; exit $(if ($Reboot) { 3 } else { 1 }) }
Remove-Item "serial.send", "serial.send.tmp" -Force -ErrorAction SilentlyContinue   # stale commands from an earlier run
$live = New-Object System.IO.StreamWriter((Join-Path $Dev "serial_live.txt"), $false, [System.Text.Encoding]::UTF8)
$live.AutoFlush = $true
try { while ([Console]::KeyAvailable) { [void][Console]::ReadKey($true) } } catch {}   # ignore keys pressed before the log started
if ($Reboot) {
  $sp.Write("reboot`n")
  $bootEnd = (Get-Date).AddSeconds(4); $booted = $false
  while (-not $booted -and (Get-Date) -lt $bootEnd) {
    try {
      $line = $sp.ReadLine(); Write-Host $line; $out.Add($line); $live.WriteLine($line)
      $booted = $line -match 'ESP-ROM:|rst:0x'
    }
    catch {}   # a timeout (no line yet), or the USB went away: not a console restart
  }
  if (-not $booted) {
    try { $sp.Close() } catch {}
    $live.Close(); Write-Host "No restart through the test console"; exit 3
  }
  Set-Content -Path "flash.status" -Value "logging" -Encoding ASCII   # (the helper waits for this script to end)
}
$end = (Get-Date).AddSeconds($Seconds)
$stopped = $false; $nextCheck = Get-Date
while ((Get-Date) -lt $end) {
  try { $line = $sp.ReadLine(); Write-Host $line; $out.Add($line); $live.WriteLine($line) } catch {}
  if ((Get-Date) -ge $nextCheck) {
    $nextCheck = (Get-Date).AddMilliseconds(200)
    if (Test-Path "stop.request") { Remove-Item "stop.request" -Force; $stopped = $true; break }
    if (Test-Path "serial.send") {
      try {
        $cmds = Get-Content "serial.send" -ErrorAction Stop
        Remove-Item "serial.send" -Force
        foreach ($c in $cmds) {
          if (-not $c.Trim()) { continue }
          $sp.Write($c.Trim() + "`n")
          $note = "> " + $c.Trim()
          Write-Host $note -ForegroundColor Cyan; $out.Add($note); $live.WriteLine($note)
        }
      } catch {}   # still being written: next check
    }
    try {
      while ([Console]::KeyAvailable) {
        $k = [Console]::ReadKey($true).Key
        if ($k -eq 'Q' -or $k -eq 'Escape') { $stopped = $true }
      }
    } catch {}
    if ($stopped) { break }
  }
}
$sp.Close()
$live.Close()
$out | Set-Content serial_log.txt
if ($stopped) { Write-Host "Stopped early."; exit 2 }
