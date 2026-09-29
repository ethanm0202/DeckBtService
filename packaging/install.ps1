<#
.SYNOPSIS
    Installs DeckBtService on a Steam Deck OLED.

.DESCRIPTION
    Run from an elevated PowerShell prompt inside the extracted release folder:

        powershell -ExecutionPolicy Bypass -File .\install.ps1

    It first creates or repairs %ProgramData%\DeckBtService with the service's own permissions (owner
    Administrators; SYSTEM and Administrators full control, Users read) and refuses a junction there.

    Steps, each logged to %ProgramData%\DeckBtService\install.log:
      1. check the hardware (Steam Deck OLED only) and the release files against SHA256SUMS, and refuse
         a DeckBtService service that runs another program (it is never replaced);
      2. install usbip-win2 0.9.8.1 (downloaded from its official GitHub release, SHA-256 pinned);
      3. switch the stock Bluetooth transport (qcbtuart.sys on ACPI\QCOM2066) off with a
         device-installation deny policy;
      4. publish the radio's UART to user mode (SerCxFriendlyName on ACPI\AMDI0020\4);
      5. copy the programs to %ProgramFiles%\DeckBtService and verify the copies against SHA256SUMS;
      6. register and start the DeckBtService service and wait until the radio is attached.

    What it changed, with prior values, is recorded in %ProgramData%\DeckBtService\install-state.json
    so that uninstall.ps1 reverses only those changes. Re-running upgrades the programs.

    Exit codes: 0 installed and running; 3010 restart Windows, then run install.ps1 again; 1 failed.

.PARAMETER DryRun
    Print every check and planned change without changing anything.
#>
[CmdletBinding()]
param(
    [switch] $DryRun
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$ServiceName = 'DeckBtService'
$InstallDir = Join-Path $env:ProgramFiles 'DeckBtService'
$DataDir = Join-Path $env:ProgramData 'DeckBtService'
$StatePath = Join-Path $DataDir 'install-state.json'
$StateTempPath = "$StatePath.tmp"
$ServiceLog = Join-Path $DataDir 'deckbt-service.log'
# The service's descriptor for its data folder (deckbt_usbip.c, OpenLog): owner Administrators, protected DACL.
$DataDirSddl = 'O:BAG:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FR;;;BU)'
$ExeFiles = @('deckbt-usbip.exe', 'deckbt-uartprobe.exe')
$ServiceExe = Join-Path $InstallDir 'deckbt-usbip.exe'
$InstallExitMessages = @{ 3 = "deckbt-usbip.exe install refused: the executable must be under $env:ProgramFiles" }

$UsbipVersion = [version]'0.9.8.1'
$UsbipUrl = 'https://github.com/vadimgrn/usbip-win2/releases/download/v.0.9.8.1/USBip-0.9.8.1-x64.exe'
$UsbipSha256 = '38CAD6D4432B52D5BB9409D9AD03B72FDFFC4ADA4CD3A48FBECA1A2752A8518A'
$UsbipExe = Join-Path $env:ProgramFiles 'USBip\usbip.exe'

$UartId = 'ACPI\AMDI0020\4'
$UartParamsKey = 'HKLM:\SYSTEM\CurrentControlSet\Enum\ACPI\AMDI0020\4\Device Parameters'
$UartFriendlyName = 'QCA2066'
# GUID_DEVINTERFACE_COMPORT
$ComPortInterface = '86e0d1e0-8089-11d0-9ce4-08003e301f73'
$PolicyKey = 'HKLM:\SOFTWARE\Policies\Microsoft\Windows\DeviceInstall\Restrictions'
$DenyListKey = Join-Path $PolicyKey 'DenyDeviceIDs'
$DenyIds = @('ACPI\QCOM2066', 'ACPI\VEN_QCOM&DEV_2066', '*QCOM2066')
$StockService = 'QcBluetooth'
$FirmwareRepository = Join-Path $env:SystemRoot 'System32\DriverStore\FileRepository'

$script:LogFile = $null
$script:State = $null
$script:DataDirHandle = $null
$script:RebootReasons = New-Object System.Collections.Generic.List[string]

# ------------------------------------------------------------------ helpers

function Write-Log {
    param([string] $Message)
    Write-Host $Message
    if ($script:LogFile) {
        Add-Content -LiteralPath $script:LogFile -Value ('{0} {1}' -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $Message) -Encoding UTF8
    }
}

# A named system change: logged, and skipped under -DryRun.
# Parameter names are unusual on purpose: the action block sees this function's variables.
function Invoke-Step {
    param([string] $StepName, [string] $StepDescription, [scriptblock] $StepAction)
    Write-Log "[$StepName] $StepDescription"
    if ($DryRun) {
        Write-Host '    (dry run: not executed)'
        return
    }
    & $StepAction
}

function Invoke-Native {
    param([string] $FilePath, [string[]] $ArgumentList, [int[]] $AllowedExitCodes = @(0), [hashtable] $ExitMessages = @{})
    # Native stderr must not become a terminating error under 'Stop'.
    $ErrorActionPreference = 'Continue'
    $output = & $FilePath @ArgumentList 2>&1 | ForEach-Object { $_.ToString() }
    $code = $LASTEXITCODE
    foreach ($line in @($output)) {
        if ($line.Trim()) { Write-Log "    $line" }
    }
    if ($AllowedExitCodes -notcontains $code) {
        if ($ExitMessages.ContainsKey($code)) { throw $ExitMessages[$code] }
        throw "$FilePath $($ArgumentList -join ' ') failed with exit code $code"
    }
    return $code
}

# One argument quoted for CommandLineToArgvW: backslashes are doubled only before a quote.
function ConvertTo-NativeArgument {
    param([string] $Argument)
    if ($Argument -and $Argument -notmatch '[\s"]') { return $Argument }
    return '"' + ($Argument -replace '(\\*)"', '$1$1\"' -replace '(\\+)$', '$1$1') + '"'
}

function Invoke-NativeTimed {
    # Invoke-Native with a deadline: returns $null after killing a process that has not exited.
    param([string] $FilePath, [string[]] $ArgumentList, [int] $TimeoutSeconds, [int[]] $AllowedExitCodes = @(0))
    $info = New-Object System.Diagnostics.ProcessStartInfo
    $info.FileName = $FilePath
    $info.Arguments = @($ArgumentList | ForEach-Object { ConvertTo-NativeArgument $_ }) -join ' '
    $info.UseShellExecute = $false
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $info.CreateNoWindow = $true
    $process = [System.Diagnostics.Process]::Start($info)
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()
    if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
        try { $process.Kill() } catch { }
        Write-Log "    $FilePath $($ArgumentList -join ' ') did not finish within $TimeoutSeconds s; stopped it"
        return $null
    }
    $process.WaitForExit()
    foreach ($line in (($stdout.Result + $stderr.Result) -split "`r?`n")) {
        if ($line.Trim()) { Write-Log "    $line" }
    }
    if ($AllowedExitCodes -notcontains $process.ExitCode) {
        throw "$FilePath $($ArgumentList -join ' ') failed with exit code $($process.ExitCode)"
    }
    return $process.ExitCode
}

function Set-BluetoothRadiosOff {
    # The Settings toggle, scripted (Windows.Devices.Radios): it closes every link and audio
    # stream. Removing the stock radio's devnode while a headset streams blocks inside PnP.
    Add-Type -AssemblyName System.Runtime.WindowsRuntime
    $asTask = [System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
        $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and
        $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1' } | Select-Object -First 1
    $wait = {
        param($Operation, [Type] $ResultType)
        $task = $asTask.MakeGenericMethod($ResultType).Invoke($null, @($Operation))
        if (-not $task.Wait(15000)) { throw 'the Windows radio API did not answer within 15 s' }
        $task.Result
    }
    [void][Windows.Devices.Radios.Radio, Windows.System.Devices, ContentType = WindowsRuntime]
    [void](& $wait ([Windows.Devices.Radios.Radio]::RequestAccessAsync()) ([Windows.Devices.Radios.RadioAccessStatus]))
    $radios = & $wait ([Windows.Devices.Radios.Radio]::GetRadiosAsync()) ([System.Collections.Generic.IReadOnlyList[Windows.Devices.Radios.Radio]])
    foreach ($radio in @($radios | Where-Object { $_.Kind -eq [Windows.Devices.Radios.RadioKind]::Bluetooth })) {
        $result = & $wait ($radio.SetStateAsync([Windows.Devices.Radios.RadioState]::Off)) ([Windows.Devices.Radios.RadioAccessStatus])
        Write-Log "    Bluetooth radio '$($radio.Name)': $($radio.State) ($result)"
    }
}

function Test-Admin {
    $principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Get-RegValue {
    param([string] $Path, [string] $Name)
    if (-not (Test-Path -LiteralPath $Path)) { return $null }
    $key = Get-Item -LiteralPath $Path
    if ($key.GetValueNames() -notcontains $Name) { return $null }
    return $key.GetValue($Name)
}

function Get-DevProp {
    param([string] $InstanceId, [string] $KeyName)
    $property = Get-PnpDeviceProperty -InstanceId $InstanceId -KeyName $KeyName -ErrorAction SilentlyContinue
    # An empty property has no Data member.
    if ($null -eq $property -or $null -eq $property.PSObject.Properties['Data']) { return $null }
    return $property.Data
}

function Get-RadioDevice {
    $device = @(Get-PnpDevice | Where-Object { $_.InstanceId -like 'ACPI\QCOM2066\*' }) | Select-Object -First 1
    if ($null -eq $device) { return $null }
    return [pscustomobject]@{
        InstanceId = $device.InstanceId
        Present    = $device.Present
        Service    = [string](Get-DevProp $device.InstanceId 'DEVPKEY_Device_Service')
        Problem    = Get-DevProp $device.InstanceId 'DEVPKEY_Device_ProblemCode'
        InfPath    = [string](Get-DevProp $device.InstanceId 'DEVPKEY_Device_DriverInfPath')
    }
}

function Format-Radio {
    param($Radio)
    $service = if ($Radio.Service) { $Radio.Service } else { 'none' }
    return "$($Radio.InstanceId): driver service '$service', problem $($Radio.Problem)"
}

function Get-BootTime {
    return (Get-CimInstance -ClassName Win32_OperatingSystem).LastBootUpTime.ToUniversalTime()
}

function Get-UsbipInstalledVersion {
    if (-not (Test-Path -LiteralPath $UsbipExe)) { return $null }
    $text = (Get-Item -LiteralPath $UsbipExe).VersionInfo.ProductVersion
    if ($text -match '^\s*(\d+(\.\d+){1,3})') { return [version]$Matches[1] }
    return $null
}

function Get-ExeVersion {
    param([string] $Path)
    if (-not (Test-Path -LiteralPath $Path)) { return $null }
    $ErrorActionPreference = 'Continue'
    $output = @(& $Path --version 2>&1 | ForEach-Object { $_.ToString() })
    # "deckbt-usbip <X.Y.Z>+<hash>[-dirty]"
    if ($LASTEXITCODE -eq 0 -and $output.Count -ge 1 -and $output[0] -match '^\S+ (\S+)') { return $Matches[1] }
    return 'unknown (no --version)'
}

function Read-SharedTail {
    param([string] $Path, [int] $MaxBytes = 262144)
    if (-not (Test-Path -LiteralPath $Path)) { return '' }
    # The running service keeps its log open for writing.
    $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]'ReadWrite, Delete')
    try {
        if ($stream.Length -gt $MaxBytes) { [void]$stream.Seek(-$MaxBytes, [IO.SeekOrigin]::End) }
        $reader = New-Object IO.StreamReader($stream)
        return $reader.ReadToEnd()
    }
    finally {
        $stream.Dispose()
    }
}

function Get-DenyPolicy {
    $entries = @()
    $foreignRestrictions = 0
    if (Test-Path -LiteralPath $DenyListKey) {
        $key = Get-Item -LiteralPath $DenyListKey
        foreach ($name in $key.GetValueNames()) {
            $entries += [pscustomobject]@{ Name = $name; Data = [string]$key.GetValue($name) }
        }
    }
    if (Test-Path -LiteralPath $PolicyKey) {
        $key = Get-Item -LiteralPath $PolicyKey
        $foreignRestrictions += @($key.GetValueNames() | Where-Object { $_ -notin @('DenyDeviceIDs', 'DenyDeviceIDsRetroactive') }).Count
        $foreignRestrictions += @($key.GetSubKeyNames() | Where-Object { $_ -ne 'DenyDeviceIDs' }).Count
    }
    return [pscustomobject]@{
        RestrictionsExists  = Test-Path -LiteralPath $PolicyKey
        ListExists          = Test-Path -LiteralPath $DenyListKey
        DenyFlag            = Get-RegValue $PolicyKey 'DenyDeviceIDs'
        Retroactive         = Get-RegValue $PolicyKey 'DenyDeviceIDsRetroactive'
        Entries             = $entries
        Ours                = @($entries | Where-Object { $DenyIds -contains $_.Data })
        Foreign             = @($entries | Where-Object { $DenyIds -notcontains $_.Data })
        ForeignRestrictions = $foreignRestrictions
    }
}

function Get-Change {
    param([string] $Type)
    return @($script:State.changes | Where-Object { $_.type -eq $Type }) | Select-Object -First 1
}

function Add-Change {
    param($Record)
    $script:State.changes = @($script:State.changes) + $Record
    Save-State
}

function Save-State {
    if ($DryRun) { return }
    $script:State.updated = (Get-Date).ToString('o')
    Write-StateFile $script:State
}

function Add-RebootReason {
    param([string] $Reason)
    Write-Log "    restart required: $Reason"
    $script:RebootReasons.Add($Reason)
    # Saved at once: if a later step fails, the next run still waits for the restart.
    $script:State.rebootPendingBoot = (Get-BootTime).ToString('o')
    Save-State
}

function Test-SameBoot {
    param([string] $BootTime)
    return [math]::Abs(((Get-BootTime) - ([datetime]$BootTime).ToUniversalTime()).TotalSeconds) -lt 10
}

# SerCx2 publishes the port (a COM port interface) only when the controller starts with SerCxFriendlyName
# set, so the value alone does not show that the port is published.
function Test-UartPublished {
    return [DeckBtPnp]::HasEnabledInterface($UartId, [guid]$ComPortInterface)
}

# ------------------------------------------------------------------ data folder

# Handle-based, as the service's SecureLogHandle: reparse points are never followed, and the folder stays
# open without delete sharing until the script ends, so its secured path cannot be swapped.
$DataDirHelperSource = @'
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.IO;
using System.Runtime.InteropServices;
using System.Security.AccessControl;
using System.Security.Principal;
using Microsoft.Win32.SafeHandles;

public static class DeckBtDataDir
{
    [StructLayout(LayoutKind.Sequential)]
    struct FileInformation { public uint Attributes, C1, C2, A1, A2, W1, W2, Volume, SizeHigh, SizeLow, Links, IndexHigh, IndexLow; }
    [StructLayout(LayoutKind.Sequential)]
    struct SecurityAttributes { public int Length; public IntPtr Descriptor; public int Inherit; }
    [StructLayout(LayoutKind.Sequential, Pack = 4)]
    struct TokenPrivilege { public int Count; public long Luid; public int Attributes; }

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern SafeFileHandle CreateFileW(string name, uint access, uint share, IntPtr security, uint disposition, uint flags, IntPtr template);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool CreateDirectoryW(string name, ref SecurityAttributes security);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern uint GetFileAttributesW(string name);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool MoveFileExW(string from, string to, uint flags);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool GetFileInformationByHandle(SafeFileHandle file, out FileInformation info);
    [DllImport("kernel32.dll")]
    static extern IntPtr LocalFree(IntPtr memory);
    [DllImport("kernel32.dll")]
    static extern IntPtr GetCurrentProcess();
    [DllImport("kernel32.dll")]
    static extern bool CloseHandle(IntPtr handle);
    [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool ConvertStringSecurityDescriptorToSecurityDescriptorW(string sddl, uint revision, out IntPtr descriptor, IntPtr size);
    [DllImport("advapi32.dll", SetLastError = true)]
    static extern bool SetSecurityDescriptorControl(IntPtr descriptor, ushort mask, ushort bits);
    [DllImport("advapi32.dll", SetLastError = true)]
    static extern bool GetKernelObjectSecurity(SafeFileHandle handle, uint information, byte[] descriptor, uint length, out uint needed);
    [DllImport("advapi32.dll", SetLastError = true)]
    static extern bool SetKernelObjectSecurity(SafeFileHandle handle, uint information, IntPtr descriptor);
    [DllImport("advapi32.dll", SetLastError = true)]
    static extern bool OpenProcessToken(IntPtr process, uint access, out IntPtr token);
    [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool LookupPrivilegeValueW(string system, string name, out long luid);
    [DllImport("advapi32.dll", SetLastError = true)]
    static extern bool AdjustTokenPrivileges(IntPtr token, bool disableAll, ref TokenPrivilege state, int length, out TokenPrivilege previous, out int needed);

    // Share modes bind only handles with a data right: ReadData (FILE_LIST_DIRECTORY on a folder) makes the
    // missing delete sharing effective, both against handles opened earlier and against later renames.
    const uint ReadData = 0x1, ReadAttributes = 0x80, ReadControl = 0x20000, WriteDac = 0x40000, WriteOwner = 0x80000;
    const uint Repair = ReadData | ReadAttributes | ReadControl | WriteDac | WriteOwner;
    const uint AttributeDirectory = 0x10, AttributeReparsePoint = 0x400;
    const uint OwnerInformation = 1, DaclInformation = 4, ProtectedDacl = 0x80000000;
    // Write data, append, write EA, delete child, write attributes, DELETE, WRITE_DAC, WRITE_OWNER, GENERIC_ALL, GENERIC_WRITE.
    const int WriteMask = 0x500D0156;
    // Backup and restore open objects whose DACL shuts Administrators out; take-ownership sets their owner.
    static readonly string[] Privileges = { "SeBackupPrivilege", "SeRestorePrivilege", "SeTakeOwnershipPrivilege" };

    // -1 when the path does not exist; a junction or symbolic link reports its own attributes.
    public static long GetAttributes(string path)
    {
        uint attributes = GetFileAttributesW(path);
        return attributes == 0xFFFFFFFF ? -1L : (long)attributes;
    }

    // Creates the folder with the descriptor, or gives it and everything below it the descriptor. Returns the
    // open folder handle. Adds to untrusted each path that was owned by or writable for anyone other than
    // SYSTEM and Administrators before the repair.
    public static SafeFileHandle Secure(string path, string sddl, List<string> untrusted)
    {
        IntPtr descriptor = Convert(sddl);
        IntPtr token = IntPtr.Zero;
        int[] restore = null;
        try {
            if (!OpenProcessToken(GetCurrentProcess(), 0x28, out token)) throw new Win32Exception();
            restore = SetPrivileges(token, null);
            SecurityAttributes attributes = new SecurityAttributes();
            attributes.Length = Marshal.SizeOf(typeof(SecurityAttributes));
            attributes.Descriptor = descriptor;
            if (!CreateDirectoryW(path, ref attributes) && Marshal.GetLastWin32Error() != 183) {
                throw new IOException("cannot create " + path + ": " + new Win32Exception().Message);
            }
            SafeFileHandle folder = Open(path);
            try {
                RepairTree(folder, path, true, descriptor, untrusted);
            }
            catch {
                folder.Dispose();
                throw;
            }
            return folder;
        }
        finally {
            if (restore != null) SetPrivileges(token, restore);
            if (token != IntPtr.Zero) CloseHandle(token);
            LocalFree(descriptor);
        }
    }

    // Gives a file the script created the descriptor; its default owner may be the user account.
    public static void SecureFile(string path, string sddl)
    {
        IntPtr descriptor = Convert(sddl);
        try {
            using (SafeFileHandle file = Open(path)) {
                Check(file, path, false);
                Apply(file, path, descriptor);
            }
        }
        finally {
            LocalFree(descriptor);
        }
    }

    // Reads a file through the handle it checked: not a link, owned by SYSTEM or Administrators, writable by
    // nobody else. Otherwise returns null and the reason. Sharing only reads keeps writers out meanwhile.
    public static string ReadTrusted(string path, out string reason)
    {
        reason = null;
        SafeFileHandle file = CreateFileW(path, ReadData | ReadAttributes | ReadControl, 1, IntPtr.Zero, 3, 0x02200000, IntPtr.Zero);
        if (file.IsInvalid) throw new IOException("cannot open " + path + ": " + new Win32Exception().Message);
        using (FileStream stream = new FileStream(file, FileAccess.Read)) {
            try {
                Check(file, path, false);
            }
            catch (IOException e) {
                reason = e.Message;
                return null;
            }
            if (!IsTrusted(file)) {
                reason = "it is owned by or writable for a non-administrator";
                return null;
            }
            using (StreamReader reader = new StreamReader(stream, true)) {
                return reader.ReadToEnd();
            }
        }
    }

    // Atomic replace that keeps the new file's descriptor (ReplaceFile would keep the old file's).
    public static void Commit(string temp, string path)
    {
        if (!MoveFileExW(temp, path, 1 | 8)) throw new IOException("cannot replace " + path + ": " + new Win32Exception().Message);
    }

    static int[] SetPrivileges(IntPtr token, int[] values)
    {
        int[] previous = new int[Privileges.Length];
        for (int i = 0; i < Privileges.Length; i++) {
            TokenPrivilege state = new TokenPrivilege();
            state.Count = 1;
            state.Attributes = values == null ? 2 : values[i];
            if (!LookupPrivilegeValueW(null, Privileges[i], out state.Luid)) throw new Win32Exception();
            TokenPrivilege old;
            int needed;
            if (!AdjustTokenPrivileges(token, false, ref state, Marshal.SizeOf(typeof(TokenPrivilege)), out old, out needed) ||
                Marshal.GetLastWin32Error() == 1300) {
                throw new Win32Exception(Marshal.GetLastWin32Error(), Privileges[i] + " is not available; run as administrator");
            }
            previous[i] = old.Count == 0 ? state.Attributes : old.Attributes;
        }
        return previous;
    }

    static IntPtr Convert(string sddl)
    {
        IntPtr descriptor;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, 1, out descriptor, IntPtr.Zero)) throw new Win32Exception();
        // SE_DACL_AUTO_INHERIT_REQ | SE_DACL_AUTO_INHERITED: the "PAI" DACL that the service's SetSecurityInfo leaves.
        if (!SetSecurityDescriptorControl(descriptor, 0x500, 0x500)) {
            int error = Marshal.GetLastWin32Error();
            LocalFree(descriptor);
            throw new Win32Exception(error);
        }
        return descriptor;
    }

    // Backup semantics, reparse point itself, share read and write only: no rename or delete while open.
    static SafeFileHandle Open(string path)
    {
        SafeFileHandle handle = CreateFileW(path, Repair, 3, IntPtr.Zero, 3, 0x02200000, IntPtr.Zero);
        if (handle.IsInvalid) throw new IOException("cannot open " + path + ": " + new Win32Exception().Message);
        return handle;
    }

    static uint Check(SafeFileHandle handle, string path, bool folder)
    {
        FileInformation info;
        if (!GetFileInformationByHandle(handle, out info)) throw new IOException(path + ": " + new Win32Exception().Message);
        if ((info.Attributes & AttributeReparsePoint) != 0) {
            throw new IOException(path + " is a junction or symbolic link; refusing to use it. Delete it as administrator (cmd /c rmdir or del), then run the script again.");
        }
        bool isFolder = (info.Attributes & AttributeDirectory) != 0;
        if (folder && !isFolder) throw new IOException(path + " is not a folder. Delete it as administrator, then run the script again.");
        if (!isFolder && info.Links != 1) {
            throw new IOException(path + " has " + info.Links + " hard links; refusing to use it. Delete it as administrator, then run the script again.");
        }
        return info.Attributes;
    }

    static bool IsAdministrative(SecurityIdentifier sid)
    {
        return sid != null && (sid.IsWellKnown(WellKnownSidType.LocalSystemSid) || sid.IsWellKnown(WellKnownSidType.BuiltinAdministratorsSid));
    }

    static bool IsTrusted(SafeFileHandle handle)
    {
        uint needed;
        GetKernelObjectSecurity(handle, OwnerInformation | DaclInformation, null, 0, out needed);
        byte[] bytes = new byte[needed];
        if (!GetKernelObjectSecurity(handle, OwnerInformation | DaclInformation, bytes, needed, out needed)) throw new Win32Exception();
        RawSecurityDescriptor descriptor = new RawSecurityDescriptor(bytes, 0);
        if (!IsAdministrative(descriptor.Owner) || descriptor.DiscretionaryAcl == null) return false;
        foreach (GenericAce ace in descriptor.DiscretionaryAcl) {
            if ((ace.AceFlags & AceFlags.InheritOnly) != 0) continue;
            QualifiedAce qualified = ace as QualifiedAce;
            if (qualified == null) return false;
            if (qualified.AceQualifier == AceQualifier.AccessAllowed && (qualified.AccessMask & WriteMask) != 0 &&
                !IsAdministrative(qualified.SecurityIdentifier)) {
                return false;
            }
        }
        return true;
    }

    // Low-level set: unlike SetSecurityInfo it does not walk the children; RepairTree does, by handle.
    static void Apply(SafeFileHandle handle, string path, IntPtr descriptor)
    {
        if (!SetKernelObjectSecurity(handle, OwnerInformation | DaclInformation | ProtectedDacl, descriptor)) {
            throw new IOException("cannot reset the owner and permissions of " + path + ": " + new Win32Exception().Message);
        }
    }

    static void RepairTree(SafeFileHandle handle, string path, bool folder, IntPtr descriptor, List<string> untrusted)
    {
        uint attributes = Check(handle, path, folder);
        if (!IsTrusted(handle)) untrusted.Add(path);
        Apply(handle, path, descriptor);
        if ((attributes & AttributeDirectory) == 0) return;
        foreach (string child in Directory.GetFileSystemEntries(path)) {
            using (SafeFileHandle childHandle = Open(child)) {
                RepairTree(childHandle, child, false, descriptor, untrusted);
            }
        }
    }
}

// Compiled with DeckBtDataDir; the scripts use it only after Initialize-DataDir.
public static class DeckBtPnp
{
    [DllImport("cfgmgr32.dll", CharSet = CharSet.Unicode)]
    static extern int CM_Get_Device_Interface_List_SizeW(out uint length, ref Guid interfaceClass, string deviceId, uint flags);
    [DllImport("cfgmgr32.dll", CharSet = CharSet.Unicode)]
    static extern int CM_Get_Device_Interface_ListW(ref Guid interfaceClass, string deviceId, char[] buffer, uint length, uint flags);

    const int BufferSmall = 0x1A;

    // Whether the device instance has an enabled interface of the class (CM_GET_DEVICE_INTERFACE_LIST_PRESENT).
    public static bool HasEnabledInterface(string deviceId, Guid interfaceClass)
    {
        for (;;) {
            uint length;
            int result = CM_Get_Device_Interface_List_SizeW(out length, ref interfaceClass, deviceId, 0);
            if (result != 0) throw new IOException("cannot list the interfaces of " + deviceId + ": CONFIGRET 0x" + result.ToString("X"));
            char[] buffer = new char[length];
            result = CM_Get_Device_Interface_ListW(ref interfaceClass, deviceId, buffer, length, 0);
            // An interface enabled between the two calls makes the buffer too small.
            if (result == BufferSmall) continue;
            if (result != 0) throw new IOException("cannot list the interfaces of " + deviceId + ": CONFIGRET 0x" + result.ToString("X"));
            return buffer.Length > 0 && buffer[0] != '\0';
        }
    }
}
'@

# Before the first read or write in it: refuses a junction or symbolic link, and creates the folder or gives
# it and everything in it the service's descriptor. Returns whether the folder exists (or would, under -DryRun).
function Initialize-DataDir {
    param([switch] $CreateMissing)
    if (-not ('DeckBtDataDir' -as [type])) { Add-Type -TypeDefinition $DataDirHelperSource -Language CSharp }
    $attributes = [DeckBtDataDir]::GetAttributes($DataDir)
    if ($attributes -ge 0 -and ($attributes -band 0x400)) {
        throw "$DataDir is a junction or symbolic link, not a folder; refusing to use it. Remove the link as administrator (cmd /c rmdir `"$DataDir`"), then run this script again."
    }
    if ($attributes -lt 0 -and -not $CreateMissing) { return $false }
    Invoke-Step 'secure-data-dir' "Create or repair $DataDir and everything in it: owner Administrators; SYSTEM and Administrators full control, Users read; no inherited permissions" {
        $untrusted = New-Object System.Collections.Generic.List[string]
        try {
            $script:DataDirHandle = [DeckBtDataDir]::Secure($DataDir, $DataDirSddl, $untrusted)
        }
        catch {
            throw $_.Exception.GetBaseException().Message
        }
        foreach ($item in $untrusted) {
            Write-Log "    repaired $item (was owned by or writable for a non-administrator)"
            # A record or its temporary copy that others could write is not trusted.
            if ($item -eq $StatePath -or $item -eq $StateTempPath) {
                Write-Log "    ignoring and deleting $item"
                Remove-Item -LiteralPath $item -Force
            }
        }
    }
    return $true
}

# Atomic: a crash leaves the previous or the new record, never a truncated one.
function Write-StateFile {
    param($State)
    $bytes = (New-Object Text.UTF8Encoding($false)).GetBytes(($State | ConvertTo-Json -Depth 8))
    # A fresh file: nobody else can hold a handle to it.
    if (Test-Path -LiteralPath $StateTempPath) { Remove-Item -LiteralPath $StateTempPath -Force }
    $stream = New-Object IO.FileStream($StateTempPath, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
    try {
        $stream.Write($bytes, 0, $bytes.Length)
        $stream.Flush($true)
    }
    finally {
        $stream.Dispose()
    }
    [DeckBtDataDir]::SecureFile($StateTempPath, $DataDirSddl)
    [DeckBtDataDir]::Commit($StateTempPath, $StatePath)
}

# $null without a trusted record. Read through the checked handle: a record others could write is ignored
# (securing the folder has deleted such a record already, unless one was planted since).
function Read-State {
    if (-not (Test-Path -LiteralPath $StatePath)) { return $null }
    $reason = $null
    $text = [DeckBtDataDir]::ReadTrusted($StatePath, [ref]$reason)
    if ($null -eq $text) {
        Write-Log "Install record: $StatePath ignored, $reason"
        return $null
    }
    $state = $text | ConvertFrom-Json
    if ($null -eq $state -or $null -eq $state.PSObject.Properties['changes']) { throw 'it has no list of changes' }
    $state.changes = @($state.changes)
    return $state
}

# ------------------------------------------------------------------ checks

function Assert-Hardware {
    $system = Get-CimInstance -ClassName Win32_ComputerSystem
    $manufacturer = "$($system.Manufacturer)".Trim()
    $model = "$($system.Model)".Trim()
    Write-Log "Hardware: manufacturer '$manufacturer', model '$model'"
    if ($manufacturer -ne 'Valve') {
        throw "This is not a Steam Deck (manufacturer '$manufacturer'). DeckBtService supports only the Steam Deck OLED."
    }
    if ($model -eq 'Jupiter') {
        throw 'This is a Steam Deck LCD (model Jupiter). Its Realtek Bluetooth radio is a USB device that Windows already drives with its own USB Bluetooth driver; DeckBtService supports only the Steam Deck OLED (model Galileo) with its Qualcomm QCA2066 radio.'
    }
    if ($model -ne 'Galileo') {
        throw "Unrecognised Steam Deck model '$model'. DeckBtService supports only the Steam Deck OLED (model Galileo)."
    }
    $radio = Get-RadioDevice
    if ($null -eq $radio -or -not $radio.Present) {
        throw 'The Qualcomm QCA2066 Bluetooth radio (ACPI\QCOM2066) was not found. DeckBtService supports only the Steam Deck OLED.'
    }
    $uart = @(Get-PnpDevice | Where-Object { $_.InstanceId -eq $UartId -and $_.Present })
    if ($uart.Count -eq 0) {
        throw "The radio's UART controller ($UartId) was not found."
    }
    Write-Log "Radio: $(Format-Radio $radio)"
    if ($radio.Service -and $radio.Service -ne $StockService) {
        throw "ACPI\QCOM2066 is bound to the driver service '$($radio.Service)', not the stock '$StockService'. Remove that driver first."
    }
    return $radio
}

function Assert-ReleaseFiles {
    $sumsPath = Join-Path $PSScriptRoot 'SHA256SUMS'
    if (-not (Test-Path -LiteralPath $sumsPath)) {
        throw "SHA256SUMS not found next to install.ps1 ($PSScriptRoot). Run install.ps1 from the extracted release folder."
    }
    # Expected SHA-256 per file name; Install-Files verifies its copies against these, not against the release folder.
    $listed = @{}
    foreach ($line in Get-Content -LiteralPath $sumsPath) {
        if (-not $line.Trim()) { continue }
        if ($line -notmatch '^([0-9A-Fa-f]{64}) [ *](\S.*)$') { throw "Malformed SHA256SUMS line: '$line'" }
        $expected = $Matches[1].ToUpperInvariant()
        $name = $Matches[2].Trim()
        if ($name -match '[\\/:]' -or $name -eq '..' -or $name -eq '.') { throw "SHA256SUMS names a file outside the release folder: '$name'" }
        $path = Join-Path $PSScriptRoot $name
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Release file missing: $name" }
        $actual = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
        if ($actual -ne $expected) { throw "Release file corrupted: $name (SHA-256 $actual, expected $expected). Download the release again." }
        $listed[$name] = $expected
    }
    foreach ($file in $ExeFiles) {
        if (-not $listed.ContainsKey($file)) { throw "SHA256SUMS does not list $file" }
    }
    Write-Log "Release files: $($listed.Count) verified against SHA256SUMS"
    return $listed
}

function Assert-Firmware {
    $dirs = @(Get-ChildItem -LiteralPath $FirmwareRepository -Directory -Filter 'qcbtuart.inf_amd64_*' -ErrorAction SilentlyContinue |
        Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'hpbtfw21.tlv') })
    if ($dirs.Count -eq 0) {
        throw "Valve's Qualcomm Bluetooth driver package (qcbtuart.inf_amd64_* with hpbtfw21.tlv) is not in the DriverStore. Install the Steam Deck OLED Bluetooth driver from Valve first; DeckBtService loads the radio firmware from it."
    }
    Write-Log "Radio firmware package: $($dirs[0].FullName)"
}

# The executable a service's command line starts with.
function Get-ServiceExePath {
    param($Config)
    if ("$($Config.PathName)" -match '^\s*"([^"]+)"') { return $Matches[1] }
    return ("$($Config.PathName)" -split '\s+')[0]
}

# A DeckBtService that runs another program is not ours to stop, delete or replace: its configuration
# (account, start type, dependencies, recovery) could not be restored on uninstall.
function Assert-ServiceName {
    $config = Get-CimInstance -ClassName Win32_Service -Filter "Name='$ServiceName'"
    if ($null -eq $config) { return }
    if ((Get-ServiceExePath $config) -ne $ServiceExe) {
        throw "A service named $ServiceName already exists and runs '$($config.PathName)', not $ServiceExe. install.ps1 does not replace another program's service; remove that service first, then run install.ps1 again."
    }
}

# ------------------------------------------------------------------ steps

function Stop-ExistingService {
    $service = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
    if ($null -eq $service) {
        Write-Log "Service: $ServiceName not registered yet"
        return
    }
    if ($service.Status -eq 'Stopped') {
        Write-Log "Service: $ServiceName registered, stopped"
        return
    }
    Invoke-Step 'stop-service' "Stop $ServiceName (detaches the radio) before changing files and drivers" {
        Stop-Service -Name $ServiceName
        (Get-Service -Name $ServiceName).WaitForStatus('Stopped', [TimeSpan]::FromSeconds(60))
    }
}

# With /NORESTART, Inno Setup reports a requested restart only in its log. It is still due unless Windows
# restarted after the installer ran.
function Request-UsbipRestart {
    param($Record)
    $setupLog = Join-Path $DataDir "usbip-$($Record.version)-install.log"
    if (-not (Test-Path -LiteralPath $setupLog)) { return }
    if (-not (Select-String -LiteralPath $setupLog -SimpleMatch 'Need to restart Windows? Yes' -Quiet)) { return }
    if (-not (Test-SameBoot $Record.startedBoot)) { return }
    Add-RebootReason 'the usbip-win2 installer asked for a restart'
}

function Install-Usbip {
    $current = Get-UsbipInstalledVersion
    $record = Get-Change 'usbip'
    if ($null -ne $current -and $current -ge $UsbipVersion) {
        Write-Log "usbip-win2: $current installed"
        if ($current -gt $UsbipVersion) { Write-Log "    note: DeckBtService was tested with $UsbipVersion" }
        # A run interrupted after starting the installer leaves its intent: the ownership recorded before the
        # installer ran still holds, and so may the restart the installer asked for.
        if ($null -ne $record -and $null -ne $record.PSObject.Properties['pending'] -and $record.pending) {
            $recordedPrior = if ($record.priorVersion) { $record.priorVersion } else { 'none' }
            Write-Log "    the previous run's usbip-win2 installation finished (prior version $recordedPrior; installed by install.ps1: $($record.installedByUs))"
            $record.pending = $false
            Save-State
            Request-UsbipRestart $record
        }
        return
    }
    $currentText = if ($null -ne $current) { "$current (too old: 0.9.8.0 and earlier break headset reconnects)" } else { 'not installed' }
    Write-Log "usbip-win2: $currentText"
    $priorText = if ($null -ne $current) { "$current" } else { 'none' }
    Invoke-Step 'install-usbip' "Download $UsbipUrl, verify SHA-256 $UsbipSha256, record the intent (prior version $priorText, target $UsbipVersion) in $StatePath, run it with /VERYSILENT /SUPPRESSMSGBOXES /NORESTART" {
        $workDir = Join-Path ([IO.Path]::GetTempPath()) ('deckbt-' + [guid]::NewGuid().ToString('N'))
        New-Item -ItemType Directory -Path $workDir | Out-Null
        $installer = Join-Path $workDir ([IO.Path]::GetFileName($UsbipUrl))
        $setupLog = Join-Path $DataDir "usbip-$UsbipVersion-install.log"
        try {
            [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
            Invoke-WebRequest -Uri $UsbipUrl -OutFile $installer -UseBasicParsing
            # Hash and run while holding a share-read handle, so the verified file cannot be swapped.
            $stream = [IO.File]::Open($installer, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
            try {
                $sha = [Security.Cryptography.SHA256]::Create()
                $actual = -join ($sha.ComputeHash($stream) | ForEach-Object { $_.ToString('X2') })
                if ($actual -ne $UsbipSha256) { throw "usbip-win2 installer SHA-256 mismatch: got $actual, expected $UsbipSha256" }
                Write-Log '    installer SHA-256 verified'
                # Recorded before the installer runs, so a rerun after an interruption knows who installed it.
                # An existing record keeps the version that was there before the first install.
                if ($null -eq $record) {
                    $record = [pscustomobject]@{
                        type          = 'usbip'
                        installedByUs = ($null -eq $current)
                        priorVersion  = if ($null -ne $current) { "$current" } else { $null }
                        version       = "$UsbipVersion"
                        sha256        = $UsbipSha256
                    }
                    $script:State.changes = @($script:State.changes) + $record
                }
                $record.version = "$UsbipVersion"
                $record.sha256 = $UsbipSha256
                $record | Add-Member -NotePropertyName pending -NotePropertyValue $true -Force
                $record | Add-Member -NotePropertyName startedBoot -NotePropertyValue (Get-BootTime).ToString('o') -Force
                Save-State
                $process = Start-Process -FilePath $installer -ArgumentList @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', "/LOG=`"$setupLog`"") -Wait -PassThru
                $exitCode = $process.ExitCode
            }
            finally {
                $stream.Dispose()
            }
        }
        finally {
            Remove-Item -LiteralPath $workDir -Recurse -Force -ErrorAction SilentlyContinue
        }
        Write-Log "    installer exit code $exitCode (setup log: $setupLog)"
        # Inno Setup 8: Preparing to Install needs a restart first; nothing was installed.
        if ($exitCode -eq 8) {
            Add-RebootReason 'the usbip-win2 installer needs a restart before it can install'
            throw [System.OperationCanceledException]'usbip-win2 setup requires a restart first'
        }
        if ($exitCode -ne 0) { throw "usbip-win2 installer failed with exit code $exitCode; see $setupLog" }
        $installed = Get-UsbipInstalledVersion
        if ($installed -ne $UsbipVersion) { throw "usbip-win2 reports version '$installed' after installation, expected $UsbipVersion" }
        $record.pending = $false
        Save-State
        Request-UsbipRestart $record
    }
}

function Disable-StockTransport {
    param($Radio)
    $policy = Get-DenyPolicy
    $record = Get-Change 'stock-transport'
    $listText = if ($policy.Entries.Count) { ($policy.Entries | ForEach-Object { "$($_.Name)=$($_.Data)" }) -join ', ' } else { 'none' }
    Write-Log "Deny policy: DenyDeviceIDs=$($policy.DenyFlag), DenyDeviceIDsRetroactive=$($policy.Retroactive), list: $listText"

    if ($null -eq $record) {
        # Deny entries for the Deck's radio with nothing else in the list come from a manual DeckBtService
        # setup: adopt them, so that uninstall restores the stock radio.
        $manual = $policy.Ours.Count -gt 0 -and $policy.Foreign.Count -eq 0
        if ($manual) { Write-Log '    existing deny entries for ACPI\QCOM2066 come from a manual setup; adopting them' }
        $record = [pscustomobject]@{
            type                   = 'stock-transport'
            restrictionsKeyCreated = (-not $policy.RestrictionsExists) -or ($manual -and $policy.ForeignRestrictions -eq 0)
            denyListKeyCreated     = (-not $policy.ListExists) -or $manual
            denyDeviceIDsPrior     = if ($manual) { $null } else { $policy.DenyFlag }
            retroactivePrior       = if ($manual) { $null } else { $policy.Retroactive }
            denyValues             = @($policy.Ours | ForEach-Object { [pscustomobject]@{ name = $_.Name; id = $_.Data; origin = 'adopted' } })
            radioInstanceId        = $Radio.InstanceId
            radioPriorService      = $Radio.Service
            radioPriorInf          = $Radio.InfPath
            radioRemoved           = $false
        }
        if (-not $DryRun) { Add-Change $record }
    }

    if (-not $policy.ListExists) {
        Invoke-Step 'deny-policy-key' "Create $DenyListKey" {
            New-Item -Path $DenyListKey -Force | Out-Null
        }
    }
    if ($policy.DenyFlag -ne 1) {
        if ($policy.Foreign.Count -gt 0) {
            $foreignText = ($policy.Foreign | ForEach-Object { "$($_.Name)=$($_.Data)" }) -join ', '
            Write-Log "    WARNING: DenyDeviceIDs is off, so enabling it also enforces $($policy.Foreign.Count) deny entries that DeckBtService did not add ($foreignText). Windows blocks installing those devices until uninstall.ps1 restores DenyDeviceIDs."
        }
        Invoke-Step 'deny-policy-enable' "Set $PolicyKey DenyDeviceIDs = 1 (DWORD)" {
            New-ItemProperty -LiteralPath $PolicyKey -Name 'DenyDeviceIDs' -Value 1 -PropertyType DWord -Force | Out-Null
        }
    }
    if ($null -eq $policy.Retroactive) {
        Invoke-Step 'deny-policy-retroactive' "Set $PolicyKey DenyDeviceIDsRetroactive = 0 (DWORD); the installed driver is removed explicitly below" {
            New-ItemProperty -LiteralPath $PolicyKey -Name 'DenyDeviceIDsRetroactive' -Value 0 -PropertyType DWord -Force | Out-Null
        }
    }
    $usedNames = @($policy.Entries | ForEach-Object { $_.Name })
    foreach ($id in $DenyIds) {
        if (@($policy.Entries | Where-Object { $_.Data -eq $id }).Count -gt 0) { continue }
        $number = 1
        while ($usedNames -contains "$number") { $number++ }
        $name = "$number"
        $usedNames += $name
        Invoke-Step 'deny-policy-entry' "Add $DenyListKey value '$name' = '$id' (REG_SZ)" {
            # Record before writing, so an interrupted run never leaves an unrecorded entry.
            $record.denyValues = @($record.denyValues) + [pscustomobject]@{ name = $name; id = $id; origin = 'added' }
            Save-State
            New-ItemProperty -LiteralPath $DenyListKey -Name $name -Value $id -PropertyType String -Force | Out-Null
        }
    }

    if (-not $Radio.Service) {
        Write-Log "Stock radio: $(Format-Radio $Radio); stock transport already off"
        return
    }
    Invoke-Step 'bluetooth-off' 'Switch Bluetooth off, as in Settings, so connected devices disconnect and release the stock radio' {
        try {
            Set-BluetoothRadiosOff
            Start-Sleep -Seconds 3
        } catch {
            Write-Log "    could not switch Bluetooth off ($($_.Exception.Message)); disconnect Bluetooth headsets by hand if the next step stalls"
        }
    }
    Invoke-Step 'remove-stock-radio' "Remove devnode $($Radio.InstanceId) (qcbtuart.sys) and rescan; with the deny policy it comes back without a driver (problem 28)" {
        $removeCode = Invoke-NativeTimed 'pnputil.exe' @('/remove-device', $Radio.InstanceId, '/subtree') -TimeoutSeconds 90 -AllowedExitCodes @(0, 3010)
        if ($null -eq $removeCode) {
            Add-RebootReason 'the stock Bluetooth radio was still in use; its removal is retried after the restart'
            return
        }
        Start-Sleep -Seconds 2
        Invoke-Native 'pnputil.exe' @('/scan-devices') | Out-Null
        Start-Sleep -Seconds 3
        $record.radioRemoved = $true
        Save-State
        $after = Get-RadioDevice
        $afterText = if ($null -ne $after) { Format-Radio $after } else { 'not enumerated yet' }
        Write-Log "    after: $afterText"
        if ($removeCode -eq 3010 -or ($null -ne $after -and $after.Service)) {
            Add-RebootReason 'the stock Bluetooth driver is released at the next restart'
        }
    }
}

# Restarts the UART controller so SerCx2 publishes the port; without a published port afterwards, a Windows
# restart is required.
function Restart-Uart {
    if ($script:RebootReasons.Count -gt 0) {
        Write-Log "    $UartId is restarted by the pending Windows restart"
        return
    }
    $code = $null
    try {
        $code = Invoke-Native 'pnputil.exe' @('/restart-device', $UartId) -AllowedExitCodes @(0, 3010)
    }
    catch {
        Write-Log "    $($_.Exception.Message)"
    }
    if ($code -eq 0) {
        $deadline = (Get-Date).AddSeconds(10)
        while (-not (Test-UartPublished) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 500 }
        if (Test-UartPublished) {
            Write-Log "    $UartId published the port"
            return
        }
    }
    Add-RebootReason "$UartId publishes the UART after a restart"
}

function Publish-Uart {
    $current = Get-RegValue $UartParamsKey 'SerCxFriendlyName'
    $record = Get-Change 'sercx-friendly-name'
    if ($current -eq $UartFriendlyName) {
        Write-Log "UART: SerCxFriendlyName = '$current' already set on $UartId"
        if ($null -eq $record -and -not $DryRun) {
            Add-Change ([pscustomobject]@{ type = 'sercx-friendly-name'; priorValue = $null; origin = 'adopted' })
        }
        if (Test-UartPublished) {
            Write-Log '    port published'
            return
        }
        # An interrupted run or a failed controller restart leaves the value written but the port unpublished.
        Write-Log '    port not published'
        Invoke-Step 'publish-uart' "Restart $UartId so SerCx2 publishes the port; if that fails, restart Windows" {
            Restart-Uart
        }
        return
    }
    $currentText = if ($null -ne $current) { "'$current'" } else { 'absent' }
    Write-Log "UART: SerCxFriendlyName $currentText"
    Invoke-Step 'publish-uart' "Set $UartParamsKey SerCxFriendlyName = '$UartFriendlyName' (REG_SZ) and restart $UartId so SerCx2 publishes the port; if that fails, restart Windows" {
        if ($null -eq $record) {
            Add-Change ([pscustomobject]@{ type = 'sercx-friendly-name'; priorValue = $current; origin = 'added' })
        }
        New-ItemProperty -LiteralPath $UartParamsKey -Name 'SerCxFriendlyName' -Value $UartFriendlyName -PropertyType String -Force | Out-Null
        Restart-Uart
    }
}

function Install-Files {
    param([hashtable] $ReleaseHashes)
    $oldVersion = Get-ExeVersion $ServiceExe
    $oldText = if ($null -ne $oldVersion) { "installed $oldVersion" } else { 'not installed yet' }
    Write-Log "Programs: $oldText; this release's version is read from its verified copy"
    $dirExisted = Test-Path -LiteralPath $InstallDir
    Invoke-Step 'install-files' "Copy $($ExeFiles -join ', ') to $InstallDir (ACL: inheritance off, SYSTEM and Administrators full control, Users read and execute) and verify the copies against SHA256SUMS" {
        if (-not $dirExisted) { New-Item -ItemType Directory -Path $InstallDir | Out-Null }
        if ($null -eq (Get-Change 'files')) {
            Add-Change ([pscustomobject]@{ type = 'files'; path = $InstallDir; dirCreated = (-not $dirExisted) })
        }
        Invoke-Native 'icacls.exe' @($InstallDir, '/reset', '/Q') | Out-Null
        Invoke-Native 'icacls.exe' @($InstallDir, '/inheritance:r', '/grant:r', '*S-1-5-18:(OI)(CI)F', '*S-1-5-32-544:(OI)(CI)F', '*S-1-5-32-545:(OI)(CI)RX', '/Q') | Out-Null
        # The release folder may be writable by other users: stage in the protected folder and check the
        # staged copies against SHA256SUMS; nothing from the release folder is executed.
        foreach ($file in $ExeFiles) {
            $staged = Join-Path $InstallDir "$file.new"
            Copy-Item -LiteralPath (Join-Path $PSScriptRoot $file) -Destination $staged -Force
            Unblock-File -LiteralPath $staged
            $actual = (Get-FileHash -LiteralPath $staged -Algorithm SHA256).Hash
            if ($actual -ne $ReleaseHashes[$file]) {
                Remove-Item -LiteralPath $staged -Force
                throw "$file in $PSScriptRoot changed after it was verified (SHA-256 $actual, expected $($ReleaseHashes[$file])). Extract the release to a folder only you can write to, then run install.ps1 again."
            }
        }
        foreach ($file in $ExeFiles) {
            Move-Item -LiteralPath (Join-Path $InstallDir "$file.new") -Destination (Join-Path $InstallDir $file) -Force
        }
        # Copied files keep only the ACEs inherited from the folder.
        Invoke-Native 'icacls.exe' @((Join-Path $InstallDir '*'), '/reset', '/Q') | Out-Null
        Write-Log "    this release: $(Get-ExeVersion $ServiceExe)"
    }
}

function Register-DeckBtService {
    # Assert-ServiceName refused a registration that runs another program.
    $config = Get-CimInstance -ClassName Win32_Service -Filter "Name='$ServiceName'"
    if ($null -ne $config) {
        Write-Log "Service: registered as $($config.PathName)"
        return
    }
    Invoke-Step 'register-service' "Register ${ServiceName}: $ServiceExe install --backend uart (LocalSystem, automatic start, restart on failure)" {
        if ($null -eq (Get-Change 'service')) {
            Add-Change ([pscustomobject]@{ type = 'service' })
        }
        Invoke-Native $ServiceExe @('install', '--backend', 'uart') -ExitMessages $InstallExitMessages | Out-Null
    }
}

function Start-DeckBtService {
    Invoke-Step 'start-service' "Start $ServiceName and wait up to 60 s for 'attach: usbip-win2 port' in $ServiceLog" {
        Start-Service -Name $ServiceName
        $deadline = (Get-Date).AddSeconds(60)
        while ((Get-Date) -lt $deadline) {
            Start-Sleep -Seconds 1
            $processId = (Get-CimInstance -ClassName Win32_Service -Filter "Name='$ServiceName'").ProcessId
            # The service renames a full log to .1: the startup and attach lines may straddle the rotation.
            $text = (Read-SharedTail "$ServiceLog.1") + (Read-SharedTail $ServiceLog)
            $start = -1
            foreach ($match in [regex]::Matches($text, 'startup (?:\S+ )?UTC \S+ pid (\d+)')) {
                if ([int]$match.Groups[1].Value -eq $processId) { $start = $match.Index }
            }
            if ($start -ge 0) {
                $attach = [regex]::Match($text.Substring($start), 'attach: usbip-win2 port \d+')
                if ($attach.Success) {
                    Write-Log "    $($attach.Value) (service pid $processId)"
                    return
                }
            }
        }
        $tail = (Read-SharedTail $ServiceLog 4096) -split "`r?`n" | Select-Object -Last 20
        foreach ($line in $tail) { Write-Log "    log: $line" }
        throw "$ServiceName did not attach the radio within 60 s. The service keeps retrying; see $ServiceLog and run collect-diagnostics.ps1."
    }
}

# ------------------------------------------------------------------ main

try {
    # 32-bit PowerShell sees Program Files (x86) as $env:ProgramFiles and has no pnputil.exe.
    if (-not [Environment]::Is64BitProcess) { throw 'Run install.ps1 from 64-bit Windows PowerShell, not Windows PowerShell (x86).' }
    if ($DryRun) { Write-Host 'DRY RUN: nothing will be changed.' }
    if (-not (Test-Admin)) {
        if (-not $DryRun) { throw 'Run install.ps1 from an elevated PowerShell prompt (Run as administrator).' }
        Write-Host 'Note: not elevated; a real run requires administrator rights.'
    }
    $radio = Assert-Hardware
    $releaseHashes = Assert-ReleaseFiles
    Assert-Firmware
    Assert-ServiceName

    Initialize-DataDir -CreateMissing | Out-Null
    if (-not $DryRun) {
        $script:LogFile = Join-Path $DataDir 'install.log'
        Write-Log "=== install.ps1 from $PSScriptRoot"
    }

    try {
        $script:State = Read-State
    }
    catch {
        throw "Install record $StatePath is unreadable ($($_.Exception.GetBaseException().Message)). Run uninstall.ps1 from this folder: it reverts the DeckBtService changes it detects and deletes the record. Then run install.ps1 again."
    }
    if ($null -ne $script:State) {
        Write-Log "Install record: $StatePath ($(@($script:State.changes).Count) recorded changes)"
    }
    else {
        $script:State = [pscustomobject]@{
            schema            = 1
            created           = (Get-Date).ToString('o')
            updated           = $null
            rebootPendingBoot = $null
            changes           = @()
        }
        Write-Log 'Install record: none yet'
    }

    if ($script:State.rebootPendingBoot) {
        if (Test-SameBoot $script:State.rebootPendingBoot) {
            Write-Host ''
            Write-Host 'A restart requested by the previous run is still pending. Restart Windows, then run install.ps1 again.'
            exit 3010
        }
    }

    Stop-ExistingService
    Install-Usbip
    Disable-StockTransport $radio
    Publish-Uart
    Install-Files $releaseHashes
    Register-DeckBtService

    if ($script:RebootReasons.Count -gt 0) {
        Write-Log ''
        Write-Log "Restart Windows to finish: $($script:RebootReasons -join '; ')."
        Write-Log "$ServiceName starts automatically after the restart. Run install.ps1 again afterwards to confirm that the radio attaches."
        exit 3010
    }

    Start-DeckBtService
    $script:State.rebootPendingBoot = $null
    Save-State
    Write-Log ''
    if ($DryRun) {
        Write-Log 'Dry run complete.'
    }
    else {
        Write-Log "DeckBtService is installed and the radio is attached. Log: $ServiceLog"
    }
    exit 0
}
catch [System.OperationCanceledException] {
    Write-Log ''
    Write-Log "Restart Windows, then run install.ps1 again ($($_.Exception.Message))."
    exit 3010
}
catch {
    $message = $_.Exception.Message
    if ($script:LogFile) { Add-Content -LiteralPath $script:LogFile -Value "$(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') ERROR: $message" -Encoding UTF8 }
    Write-Host ''
    Write-Host "ERROR: $message" -ForegroundColor Red
    exit 1
}
