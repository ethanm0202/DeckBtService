<#
.SYNOPSIS
    Collects DeckBtService diagnostics into a zip on the Desktop.

.DESCRIPTION
    Run it after a problem, preferably right away. collect-diagnostics.cmd runs it as administrator;
    from a PowerShell prompt:

        powershell -ExecutionPolicy Bypass -File .\collect-diagnostics.ps1

    Without administrator rights a few items (usbip.exe port) may be incomplete. The zip goes to the
    Desktop of the account that runs it, which is the administrator account if a different one was used
    to elevate. The zip holds the DeckBtService logs and install record, the service configuration,
    usbip-win2's version, ports and driver packages, the state of the radio-related devices, the
    paired Bluetooth devices (name, address in the device ID, connected flag), the relevant
    registry settings, and the Windows build and BIOS product. User profile paths (C:\Users\<account>) in
    the copied logs are replaced with %USERPROFILE%; other file and folder paths stay.
    Bluetooth link keys (BTHPORT\Parameters\Keys) are never read.
#>
[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

# 32-bit PowerShell sees Program Files (x86) as $env:ProgramFiles and has no pnputil.exe.
if (-not [Environment]::Is64BitProcess) { throw 'Run collect-diagnostics.ps1 from 64-bit Windows PowerShell, not Windows PowerShell (x86).' }

$ServiceName = 'DeckBtService'
$DataDir = Join-Path $env:ProgramData 'DeckBtService'
$InstallDir = Join-Path $env:ProgramFiles 'DeckBtService'
$UsbipExe = Join-Path $env:ProgramFiles 'USBip\usbip.exe'
$UsbipUninstallKey = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\{199505b0-b93d-4521-a8c7-897818e0205a}_is1'
$UartParamsKey = 'HKLM:\SYSTEM\CurrentControlSet\Enum\ACPI\AMDI0020\4\Device Parameters'
$PolicyKey = 'HKLM:\SOFTWARE\Policies\Microsoft\Windows\DeviceInstall\Restrictions'
$ComPortInterfaces = 'HKLM:\SYSTEM\CurrentControlSet\Control\DeviceClasses\{86e0d1e0-8089-11d0-9ce4-08003e301f73}'

# Profile folders name the Windows account: every known profile path, then any other C:\Users\<name>
# (for example 8.3 short names in setup logs), becomes %USERPROFILE%.
$ProfileListKey = 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\ProfileList'
$ProfilesRoot = [Environment]::ExpandEnvironmentVariables((Get-ItemProperty -LiteralPath $ProfileListKey).ProfilesDirectory)
$ProfilePaths = @(@($env:USERPROFILE) + @(Get-ChildItem -LiteralPath $ProfileListKey | ForEach-Object { [Environment]::ExpandEnvironmentVariables([string]$_.GetValue('ProfileImagePath')) }) |
    Where-Object { $_ -and $_.StartsWith("$ProfilesRoot\", [StringComparison]::OrdinalIgnoreCase) } | Sort-Object -Unique | Sort-Object -Property Length -Descending)
$ProfilePattern = [regex]::Escape($ProfilesRoot) + '\\[^\\/:*?"<>|\s]+'

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$desktop = [Environment]::GetFolderPath('Desktop')
$zipPath = Join-Path $desktop "DeckBtService-diagnostics-$stamp.zip"
$work = Join-Path ([IO.Path]::GetTempPath()) "DeckBtService-diagnostics-$stamp"
New-Item -ItemType Directory -Path $work | Out-Null

# Each section is written to its own file; a failing section records its error and the rest continue.
function Write-Section {
    param([string] $FileName, [scriptblock] $Body)
    $ErrorActionPreference = 'Continue'
    $path = Join-Path $work $FileName
    try {
        $text = & $Body 2>&1 | Out-String -Width 400
    }
    catch {
        $text = "ERROR: $($_.Exception.Message)"
    }
    Set-Content -LiteralPath $path -Value $text -Encoding UTF8
    Write-Host "  $FileName"
}

function Invoke-Native {
    param([string] $FilePath, [string[]] $ArgumentList)
    $ErrorActionPreference = 'Continue'
    "> $FilePath $($ArgumentList -join ' ')"
    & $FilePath @ArgumentList 2>&1 | ForEach-Object { $_.ToString() }
    "(exit code $LASTEXITCODE)"
    ''
}

function Hide-ProfilePaths {
    param([string] $Text)
    foreach ($path in $ProfilePaths) { $Text = $Text -ireplace [regex]::Escape($path), '%USERPROFILE%' }
    return $Text -ireplace $ProfilePattern, '%USERPROFILE%'
}

function Copy-SharedFile {
    param([string] $Source, [string] $Destination)
    # The running service keeps its log open for writing.
    $in = [IO.File]::Open($Source, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]'ReadWrite, Delete')
    try {
        $text = (New-Object IO.StreamReader($in, $true)).ReadToEnd()
    }
    finally {
        $in.Dispose()
    }
    [IO.File]::WriteAllText($Destination, (Hide-ProfilePaths $text), (New-Object Text.UTF8Encoding($false)))
}

# One query per device: batched queries over several devices mislabel their results.
function Get-DevProps {
    param([object[]] $Devices, [string[]] $KeyNames)
    $table = @{}
    foreach ($device in $Devices) {
        foreach ($property in @(Get-PnpDeviceProperty -InstanceId $device.InstanceId -KeyName $KeyNames -ErrorAction SilentlyContinue)) {
            # An empty property has no Data member.
            if ($null -ne $property.PSObject.Properties['Data']) {
                $table["$($device.InstanceId)|$($property.KeyName)"] = $property.Data
            }
        }
    }
    return $table
}

function Format-Devices {
    param([object[]] $Devices)
    $props = Get-DevProps $Devices @('DEVPKEY_Device_ProblemCode', 'DEVPKEY_Device_Service', 'DEVPKEY_Device_DriverInfPath', 'DEVPKEY_Device_DriverVersion')
    foreach ($device in $Devices) {
        $id = $device.InstanceId
        [pscustomobject]@{
            InstanceId = $id
            Name       = $device.FriendlyName
            Class      = $device.Class
            Present    = $device.Present
            Status     = $device.Status
            Problem    = $props["$id|DEVPKEY_Device_ProblemCode"]
            Service    = $props["$id|DEVPKEY_Device_Service"]
            Inf        = $props["$id|DEVPKEY_Device_DriverInfPath"]
            Driver     = $props["$id|DEVPKEY_Device_DriverVersion"]
        }
    }
}

Write-Host "Collecting DeckBtService diagnostics into $work"
$devices = @(Get-PnpDevice)

Write-Section 'files.txt' {
    foreach ($source in @(Get-ChildItem -LiteralPath $DataDir -File -ErrorAction SilentlyContinue)) {
        # Logs, the install record and setup logs; downloaded installers are not copied.
        if ($source.Extension -in @('.log', '.1', '.json')) {
            try {
                Copy-SharedFile $source.FullName (Join-Path $work $source.Name)
                "copied $($source.FullName) ($($source.Length) bytes)"
            }
            catch {
                "cannot copy $($source.FullName): $($_.Exception.Message)"
            }
        }
    }
    ''
    "Program folder $InstallDir"
    Get-ChildItem -LiteralPath $InstallDir -File -ErrorAction SilentlyContinue |
        ForEach-Object { '{0}  {1}  {2}' -f $_.Name, $_.Length, (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash }
}

Write-Section 'service.txt' {
    Invoke-Native 'sc.exe' @('qc', $ServiceName)
    Invoke-Native 'sc.exe' @('queryex', $ServiceName)
    Invoke-Native 'sc.exe' @('qfailure', $ServiceName)
    $exe = Join-Path $InstallDir 'deckbt-usbip.exe'
    if (Test-Path -LiteralPath $exe) { Invoke-Native $exe @('--version') } else { "$exe not found" }
}

Write-Section 'usbip.txt' {
    'Installed usbip-win2'
    if (Test-Path -LiteralPath $UsbipExe) { "usbip.exe version $((Get-Item -LiteralPath $UsbipExe).VersionInfo.ProductVersion)" } else { "$UsbipExe not found" }
    Get-ItemProperty -LiteralPath $UsbipUninstallKey -ErrorAction SilentlyContinue | Select-Object DisplayName, DisplayVersion, InstallLocation | Format-List
    if (Test-Path -LiteralPath $UsbipExe) { Invoke-Native $UsbipExe @('port') }
    'Driver packages (usbip, qcbtuart, deckbt)'
    $blocks = (Invoke-Native 'pnputil.exe' @('/enum-drivers') | Out-String) -split '(?:\r?\n){2,}'
    $blocks | Where-Object { $_ -match '(?i)usbip|qcbtuart|deckbt' }
}

Write-Section 'devices.txt' {
    'Stock radio (ACPI\QCOM2066) and its UART (ACPI\AMDI0020)'
    Format-Devices @($devices | Where-Object { $_.InstanceId -like 'ACPI\QCOM2066\*' -or $_.InstanceId -like 'ACPI\AMDI0020\*' }) | Format-List
    'Virtual adapter (USB\VID_0CF3&PID_6390)'
    Format-Devices @($devices | Where-Object { $_.InstanceId -like 'USB\VID_0CF3&PID_6390*' }) | Format-List
    'Bluetooth enumerators and children (BTH*)'
    @($devices | Where-Object { $_.InstanceId -like 'BTH*' }) |
        Sort-Object -Property InstanceId | Format-Table -Property Present, Status, Problem, Class, FriendlyName, InstanceId -AutoSize
    'Other Bluetooth-class devices'
    Format-Devices @($devices | Where-Object { $_.Class -eq 'Bluetooth' -and $_.InstanceId -notlike 'BTH*' -and $_.InstanceId -notlike 'USB\VID_0CF3&PID_6390*' }) |
        Format-Table -Property Present, Status, Problem, Service, Name, InstanceId -AutoSize
}

Write-Section 'paired-devices.txt' {
    $paired = @($devices | Where-Object { $_.InstanceId -like 'BTHENUM\DEV_*' -or $_.InstanceId -like 'BTHLE\DEV_*' })
    # Boolean "connected" property that Windows keeps on BTHENUM\DEV_* and BTHLE\DEV_* nodes.
    $connectedKey = '{83DA6326-97A6-4088-9453-A1923F573B29} 15'
    $props = Get-DevProps $paired @($connectedKey)
    $rows = foreach ($device in $paired) {
        [pscustomobject]@{
            Name       = $device.FriendlyName
            Transport  = if ($device.InstanceId -like 'BTHLE\*') { 'LE' } else { 'Classic' }
            Connected  = $props["$($device.InstanceId)|$connectedKey"]
            Present    = $device.Present
            InstanceId = $device.InstanceId
        }
    }
    $rows | Format-Table -AutoSize
}

Write-Section 'registry.txt' {
    "$UartParamsKey"
    Get-ItemProperty -LiteralPath $UartParamsKey -ErrorAction SilentlyContinue | Select-Object -Property SerCxFriendlyName | Format-List
    'Published COM port interfaces of ACPI\AMDI0020'
    Get-ChildItem -LiteralPath $ComPortInterfaces -ErrorAction SilentlyContinue | Where-Object { $_.PSChildName -like '*AMDI0020*' } | ForEach-Object { $_.PSChildName }
    ''
    "$PolicyKey (recursive)"
    Invoke-Native 'reg.exe' @('query', 'HKLM\SOFTWARE\Policies\Microsoft\Windows\DeviceInstall\Restrictions', '/s')
}

Write-Section 'system.txt' {
    Get-CimInstance -ClassName Win32_OperatingSystem | Select-Object -Property Caption, Version, BuildNumber, OSArchitecture, LastBootUpTime | Format-List
    "UBR: $((Get-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion').UBR)"
    Get-CimInstance -ClassName Win32_ComputerSystem | Select-Object -Property Manufacturer, Model, SystemFamily | Format-List
    Get-CimInstance -ClassName Win32_BIOS | Select-Object -Property Manufacturer, SMBIOSBIOSVersion, ReleaseDate | Format-List
    Get-CimInstance -ClassName Win32_BaseBoard | Select-Object -Property Manufacturer, Product | Format-List
    "Collected $(Get-Date -Format o); elevated: $(([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator))"
}

Compress-Archive -Path (Join-Path $work '*') -DestinationPath $zipPath
Remove-Item -LiteralPath $work -Recurse -Force
Write-Host ''
Write-Host "Diagnostics saved to $zipPath"
Write-Host 'Attach it to your bug report. It contains the names and addresses of your paired Bluetooth devices, but no pairing keys.'
Write-Host 'User profile paths in the logs are replaced with %USERPROFILE%; other file and folder paths stay and may still show your Windows account name.'
