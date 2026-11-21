# Closes the main frame of a running Sandbox like File > Exit (WM_CLOSE through UI Automation), N seconds after the
# frame appeared. Companion of smoke_test.sh for runs where tool windows are floating: smoke_dialog_watch.ps1 decides by
# Process.MainWindowTitle, which Windows then reports as the title of a floating tool window ("CEV Video Editor"), so its
# -CloseAfter never fires and the editor stays open.
#   powershell -File tools/smoke_close_main.ps1 -After 20 [-MaxSeconds 600] [-ProcName Sandbox]
param([int]$After = 20, [int]$MaxSeconds = 600, [string]$ProcName = 'Sandbox')
Add-Type -AssemblyName UIAutomationClient
$deadline = (Get-Date).AddSeconds($MaxSeconds)
$seen = $null
$root = [System.Windows.Automation.AutomationElement]::RootElement
while ((Get-Date) -lt $deadline) {
  Start-Sleep -Milliseconds 1000
  $procs = @(Get-Process -Name $ProcName -ErrorAction SilentlyContinue)
  if ($procs.Count -eq 0) { if ($seen) { exit 0 } else { continue } }
  foreach ($p in $procs) {
    $c = New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::ProcessIdProperty, [int]$p.Id)
    foreach ($e in $root.FindAll([System.Windows.Automation.TreeScope]::Children, $c)) {
      if ($e.Current.Name -like 'CRYENGINE Sandbox*') {
        if (-not $seen) { $seen = Get-Date }
        elseif (((Get-Date) - $seen).TotalSeconds -ge $After) {
          try { $e.GetCurrentPattern([System.Windows.Automation.WindowPattern]::Pattern).Close() } catch { }
          $seen = (Get-Date).AddSeconds(15 - $After) # try again in 15 s if the frame is still there
        }
      }
    }
  }
}
exit 1
