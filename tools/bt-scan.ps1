<#
    bt-scan.ps1 - RF discovery through the native Windows Bluetooth stack.

    Runs whatever radio Windows currently uses; no device, driver or registry changes and no
    elevation. Asks Windows device discovery (Windows.Devices.Enumeration, association endpoints)
    for Bluetooth LE and Bluetooth Classic devices; for these protocols Windows answers by scanning
    and inquiring over the air. Windows PowerShell cannot subscribe to WinRT events, so the one-shot
    FindAllAsync is used rather than an advertisement watcher.

    A device counts as heard over the air only with System.Devices.Aep.IsPresent = true. Paired
    devices can report present from cached Windows state without live radio traffic, so only heard
    devices that are not paired represent active over-the-air discovery.
    Prints 'SCAN RESULT: le=<n> classic=<n> unpaired=<n>' (unpaired = distinct heard, not paired
    addresses) and exits 0 when unpaired > 0, 1 when none, 2 when discovery did not finish within
    -TimeoutSeconds.
#>
[CmdletBinding()]
param([ValidateRange(5, 300)][int] $TimeoutSeconds = 60)
$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName System.Runtime.WindowsRuntime
$null = [Windows.Devices.Enumeration.DeviceInformation, Windows.Devices.Enumeration, ContentType = WindowsRuntime]
$null = [Windows.Devices.Enumeration.DeviceInformationCollection, Windows.Devices.Enumeration, ContentType = WindowsRuntime]
$asTask = [System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
    $_.Name -eq 'AsTask' -and $_.IsGenericMethod -and $_.GetParameters().Count -eq 1 -and
    $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1'
} | Select-Object -First 1
$collectionType = [Windows.Devices.Enumeration.DeviceInformationCollection]

$protocols = [ordered]@{
    le      = '{bb7bb05e-5972-42b5-94fc-76eaa7084d49}'
    classic = '{e0cbf06c-cd8b-4647-bb8a-263b43f0f974}'
}
$props = [string[]]@('System.Devices.Aep.DeviceAddress', 'System.Devices.Aep.IsPresent',
                     'System.Devices.Aep.IsPaired', 'System.Devices.Aep.SignalStrength')
$kind = [Windows.Devices.Enumeration.DeviceInformationKind]::AssociationEndpoint

# Both discoveries run concurrently; each is one bounded wait.
$tasks = [ordered]@{}
foreach ($name in $protocols.Keys) {
    $aqs = 'System.Devices.Aep.ProtocolId:="' + $protocols[$name] + '"'
    $op = [Windows.Devices.Enumeration.DeviceInformation]::FindAllAsync($aqs, $props, $kind)
    $tasks[$name] = $asTask.MakeGenericMethod($collectionType).Invoke($null, @($op))
}
$started = [DateTime]::UtcNow
$counts = @{}
$exit = 1
$unpaired = @{}
foreach ($name in $tasks.Keys) {
    $left = [Math]::Max(1, $TimeoutSeconds - [int]([DateTime]::UtcNow - $started).TotalSeconds)
    if (-not $tasks[$name].Wait($left * 1000)) {
        Write-Host "$name discovery did not finish within ${TimeoutSeconds}s."
        $exit = 2
        $counts[$name] = 0
        continue
    }
    # PowerShell does not project the WinRT property map's indexer; copy the pairs instead.
    $all = @($tasks[$name].Result | ForEach-Object {
        $p = @{}
        foreach ($kv in $_.Properties) { $p[$kv.Key] = $kv.Value }
        [pscustomobject]@{
            Name = $_.Name; Address = $p['System.Devices.Aep.DeviceAddress']; Present = $p['System.Devices.Aep.IsPresent']
            Paired = $p['System.Devices.Aep.IsPaired']; Rssi = $p['System.Devices.Aep.SignalStrength']
        }
    })
    $heard = @($all | Where-Object { $_.Present -eq $true })
    $counts[$name] = $heard.Count
    foreach ($d in ($heard | Where-Object { $_.Paired -ne $true })) { $unpaired[[string]$d.Address] = $true }
    Write-Host ("{0} ENDPOINTS: {1} listed, {2} heard" -f $name.ToUpper(), $all.Count, $heard.Count)
    foreach ($d in $all) {
        Write-Host ('  {0}  present={1,-5} paired={2,-5} rssi={3,4}  {4}' -f $d.Address, $d.Present, $d.Paired, $d.Rssi, $d.Name)
    }
}
Write-Host ("SCAN RESULT: le={0} classic={1} unpaired={2} ({3:N1}s)" -f $counts['le'], $counts['classic'], $unpaired.Count, ([DateTime]::UtcNow - $started).TotalSeconds)
if ($exit -ne 2 -and $unpaired.Count -gt 0) { $exit = 0 }
exit $exit
