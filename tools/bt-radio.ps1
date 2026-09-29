<#
    bt-radio.ps1 - the Settings "Bluetooth" toggle, scripted (Windows.Devices.Radios).

    -State Off disconnects every Bluetooth device without touching any devnode. A connected HID
    device (such as a Bluetooth mouse exposing a keyboard collection) can cause disabling the
    radio devnode to require a reboot; switching the radio off first removes those collections.
    -State On switches it back. Without -State, prints the current state. Exits 0 when the radio
    ends in the requested state.
#>
param([ValidateSet('On', 'Off')][string] $State)
$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName System.Runtime.WindowsRuntime
$asTask = [System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
    $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and
    $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1' } | Select-Object -First 1
function Wait-WinRt($Operation, [Type] $ResultType) {
    $task = $asTask.MakeGenericMethod($ResultType).Invoke($null, @($Operation))
    if (-not $task.Wait(15000)) { throw 'WinRT operation timed out.' }
    $task.Result
}
[void][Windows.Devices.Radios.Radio, Windows.System.Devices, ContentType = WindowsRuntime]

$access = Wait-WinRt ([Windows.Devices.Radios.Radio]::RequestAccessAsync()) ([Windows.Devices.Radios.RadioAccessStatus])
$radios = Wait-WinRt ([Windows.Devices.Radios.Radio]::GetRadiosAsync()) ([System.Collections.Generic.IReadOnlyList[Windows.Devices.Radios.Radio]])
$bt = @($radios | Where-Object { $_.Kind -eq [Windows.Devices.Radios.RadioKind]::Bluetooth })
if ($bt.Count -eq 0) {
    Write-Host "BT RADIO: none present (access=$access)"
    exit $(if ($State) { 1 } else { 0 })
}
$ok = $true
foreach ($r in $bt) {
    if ($State) {
        $target = [Windows.Devices.Radios.RadioState]::$State
        $result = Wait-WinRt ($r.SetStateAsync($target)) ([Windows.Devices.Radios.RadioAccessStatus])
        Start-Sleep -Milliseconds 500
        if ($r.State -ne $target) { $ok = $false }
        Write-Host ("BT RADIO: {0} -> {1} (set={2}, access={3})" -f $r.Name, $r.State, $result, $access)
    } else {
        Write-Host ("BT RADIO: {0} is {1} (access={2})" -f $r.Name, $r.State, $access)
    }
}
exit $(if ($ok) { 0 } else { 1 })
