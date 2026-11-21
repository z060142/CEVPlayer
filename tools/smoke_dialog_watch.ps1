# Companion of smoke_test.sh: watches a process for a blocking error dialog (CryMessageBox / CRYENGINE
# FATAL ERROR / crash dialog / "has stopped working"), writes the dialog text to <out> and kills the
# process so the smoke test fails fast instead of waiting for the hard timeout.
# With -CloseAfter N (Sandbox mode) it also closes the editor main window (WM_CLOSE) N seconds after the
# window appears: the editor skips the console deferred queue, so there is no scripted `quit` there.
#   powershell -File smoke_dialog_watch.ps1 -ProcName GameLauncher -OutFile dialog.txt -MaxSeconds 600
param([string]$ProcName, [string]$OutFile, [string]$LogFile = "", [int]$CloseAfter = 0, [int]$MaxSeconds = 600, [string]$Pattern = 'ERROR|Error|error|crash|Crash|stopped working|Fatal|FATAL|Assert|Exception|Warning')
Add-Type -AssemblyName UIAutomationClient
$deadline = (Get-Date).AddSeconds($MaxSeconds)
while ((Get-Date) -lt $deadline) {
  Start-Sleep -Milliseconds 1500
  $procs = @(Get-Process -Name $ProcName -ErrorAction SilentlyContinue)
  if ($procs.Count -eq 0 -and (Test-Path "$OutFile.started")) { exit 0 }
  if ($procs.Count -gt 0) { Set-Content "$OutFile.started" '1' }
  if ($CloseAfter -gt 0) {
    foreach ($p in $procs) {
      if ($p.MainWindowHandle -ne 0 -and $p.MainWindowTitle -match 'Sandbox') {
        if (-not $script:seen) { $script:seen = Get-Date; Set-Content "$OutFile.window" $p.MainWindowTitle }
        elseif (((Get-Date) - $script:seen).TotalSeconds -ge $CloseAfter -and (-not $script:lastClose -or ((Get-Date) - $script:lastClose).TotalSeconds -ge 15)) {
          $script:lastClose = Get-Date
          [void]$p.CloseMainWindow()   # WM_CLOSE: the editor's normal File > Exit path
        }
      }
    }
  }
  if ($LogFile -and (Test-Path $LogFile)) {
    $hit = Get-Content $LogFile -Tail 20 -ErrorAction SilentlyContinue | Select-String -Pattern 'Saved dump file|CRYENGINE has crashed|Unhandled exception'
    if ($hit) {
      Set-Content -Path $OutFile -Value @("crash handler ran: " + $hit[0].Line.Trim()) -Encoding UTF8
      Start-Sleep -Seconds 3
      foreach ($p in $procs) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
      exit 0
    }
  }
  foreach ($p in $procs) {
    $t = $p.MainWindowTitle
    if ($t -and $t -match $Pattern -and $t -notmatch '^CRYENGINE( Launcher)?$') {
      $root = [System.Windows.Automation.AutomationElement]::RootElement
      $c = New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::ProcessIdProperty, [int]$p.Id)
      $lines = @("title: $t")
      foreach ($e in $root.FindAll([System.Windows.Automation.TreeScope]::Descendants, $c)) { if ($e.Current.Name) { $lines += $e.Current.Name } }
      Set-Content -Path $OutFile -Value $lines -Encoding UTF8
      Stop-Process -Id $p.Id -Force
      exit 0
    }
  }
}
