# Write a raw disk image to a removable card on Windows, safely.
#
# WHY this exists: writing to the wrong \\.\PhysicalDrive wipes a disk. This script
# refuses anything that is not a small USB/SD device (2-128 GB, not boot/system), and
# works around the Windows gotcha where removable media cannot be set offline and the
# OS re-mounts the partition mid-write ("Access denied"): it disables automount and
# cleans the disk with diskpart first.
#
# USAGE: edit $disk and $img below, then run ELEVATED (admin):
#   - find the card's disk number:  Get-Disk | ? BusType -in 'USB','SD'
#   - $img must be a DECOMPRESSED .img (decompress .img.xz first).
#   powershell -ExecutionPolicy Bypass -File flash_sd.ps1
$ErrorActionPreference = 'Stop'
$log  = "$PSScriptRoot\flash_sd.log"
$disk = 4                                   # <-- verify with Get-Disk before running
$img  = 'C:\path\to\image.img'              # <-- decompressed image to write
function L($m){ "$([DateTime]::Now.ToString('HH:mm:ss')) $m" | Tee-Object -FilePath $log -Append | Out-Null }
Set-Content $log "START $([DateTime]::Now)"
try {
    $d = Get-Disk -Number $disk
    L "Target disk $disk : $($d.FriendlyName) Bus=$($d.BusType) SizeGB=$([math]::Round($d.Size/1GB,1)) Boot=$($d.IsBoot) System=$($d.IsSystem)"
    # SAFETY: refuse anything that is not a small removable USB disk
    if ($d.BusType -notin @('USB','SD'))      { throw "ABORT: disk $disk is $($d.BusType), not USB/SD" }
    if ($d.IsBoot -or $d.IsSystem)            { throw "ABORT: disk $disk is boot/system" }
    if ($d.Size -gt 128GB -or $d.Size -lt 2GB){ throw "ABORT: disk $disk size $([math]::Round($d.Size/1GB,1))GB outside 2-128GB" }
    $len = (Get-Item $img).Length
    L "Image $img = $len bytes"

    L "Disabling automount and cleaning disk via diskpart"
    $dp = "$env:TEMP\dp_$disk.txt"
    @("automount disable","select disk $disk","clean","exit") | Set-Content -Encoding Ascii $dp
    $out = & diskpart /s $dp 2>&1 | Out-String
    L ("diskpart: " + ($out -replace '\s+',' ').Trim())
    Remove-Item $dp -ErrorAction SilentlyContinue
    try { Set-Disk -Number $disk -IsReadOnly $false -ErrorAction Stop } catch { L "note: set readonly false: $_" }
    Start-Sleep -Seconds 2

    $src = [System.IO.File]::OpenRead($img)
    $dst = New-Object System.IO.FileStream("\\.\PhysicalDrive$disk",[IO.FileMode]::Open,[IO.FileAccess]::ReadWrite,[IO.FileShare]::ReadWrite)
    $buf = New-Object byte[] (4MB)
    $total = 0L; $sw = [Diagnostics.Stopwatch]::StartNew(); $lastLog = 0
    while (($n = $src.Read($buf,0,$buf.Length)) -gt 0) {
        if ($n -lt $buf.Length) { $pad = 512 - ($n % 512); if ($pad -ne 512){ for($i=$n;$i -lt $n+$pad;$i++){$buf[$i]=0}; $n += $pad } }
        $dst.Write($buf,0,$n); $total += $n
        if ($total - $lastLog -ge 100MB) { $lastLog = $total; L ("written {0} MB ({1} MB/s)" -f [math]::Round($total/1MB),[math]::Round($total/1MB/$sw.Elapsed.TotalSeconds,1)) }
    }
    $dst.Flush(); $dst.Close(); $src.Close()
    L "FLUSH done, total $total bytes in $([math]::Round($sw.Elapsed.TotalSeconds))s"

    # read-back verify first 64 MB
    L "Verifying first 64 MB"
    $s2 = [System.IO.File]::OpenRead($img)
    $d2 = New-Object System.IO.FileStream("\\.\PhysicalDrive$disk",[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::ReadWrite)
    $a = New-Object byte[] (1MB); $b = New-Object byte[] (1MB); $ok=$true
    for ($i=0; $i -lt 64; $i++) {
        $na=$s2.Read($a,0,$a.Length); $nb=$d2.Read($b,0,$b.Length)
        if ($na -ne $nb -or [System.Linq.Enumerable]::SequenceEqual($a,$b) -eq $false) { $ok=$false; L "MISMATCH at MB $i"; break }
    }
    $s2.Close(); $d2.Close()
    L ("VERIFY " + ($(if($ok){'OK'}else{'FAILED'})))
    L "DONE"
} catch {
    L "ERROR: $_"
} finally {
    $dp2 = "$env:TEMP\dp_en.txt"
    @("automount enable","exit") | Set-Content -Encoding Ascii $dp2
    & diskpart /s $dp2 2>&1 | Out-Null
    Remove-Item $dp2 -ErrorAction SilentlyContinue
}
