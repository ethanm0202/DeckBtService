<#
.SYNOPSIS
    Removes DeckBtService and returns the Steam Deck OLED's Bluetooth radio to the stock driver.

.DESCRIPTION
    Run from an elevated PowerShell prompt:

        powershell -ExecutionPolicy Bypass -File .\uninstall.ps1

    Stops and removes the DeckBtService service (only the one install.ps1 registers, running
    %ProgramFiles%\DeckBtService\deckbt-usbip.exe), then reverses the changes recorded by install.ps1
    in %ProgramData%\DeckBtService\install-state.json, newest first: the program folder, the
    published UART (SerCxFriendlyName) and the stock-transport deny policy, after which the stock
    driver (qcbtuart.sys) is installed on ACPI\QCOM2066 again. Without a readable install record (a
    manual setup) it detects those changes and reverts them. Before reading anything in
    %ProgramData%\DeckBtService it gives that folder the service's own permissions and refuses a junction there.

    Exit codes: 0 done; 3010 done, restart Windows to finish; 1 failed.

.PARAMETER RemoveUsbip
    Also run usbip-win2's own uninstaller, if install.ps1 installed usbip-win2.

.PARAMETER RemoveLogs
    Also delete the DeckBtService logs in %ProgramData%\DeckBtService (kept by default); other files stay.

.PARAMETER DryRun
    Print every check and planned change without changing anything.
#>
[CmdletBinding()]
param(
    [switch] $RemoveUsbip,
    [switch] $RemoveLogs,
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
# The service's descriptor for its data folder (deckbt_usbip.c, OpenLog): owner Administrators, protected DACL.
$DataDirSddl = 'O:BAG:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FR;;;BU)'
# Files the scripts and the service write there; -RemoveLogs deletes only these and usbip-<version>-install.log.
$LogNames = @('install.log', 'uninstall.log', 'deckbt-service.log', 'deckbt-service.log.1', 'usbip-uninstall.log')
$ServiceExe = Join-Path $InstallDir 'deckbt-usbip.exe'

$UsbipUninstallKey = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\{199505b0-b93d-4521-a8c7-897818e0205a}_is1'

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
    param([string] $FilePath, [string[]] $ArgumentList, [int[]] $AllowedExitCodes = @(0))
    # Native stderr must not become a terminating error under 'Stop'.
    $ErrorActionPreference = 'Continue'
    $output = & $FilePath @ArgumentList 2>&1 | ForEach-Object { $_.ToString() }
    $code = $LASTEXITCODE
    foreach ($line in @($output)) {
        if ($line.Trim()) { Write-Log "    $line" }
    }
    if ($AllowedExitCodes -notcontains $code) {
        throw "$FilePath $($ArgumentList -join ' ') failed with exit code $code"
    }
    return $code
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

function Test-KeyEmpty {
    param([string] $Path)
    $key = Get-Item -LiteralPath $Path
    return $key.ValueCount -eq 0 -and $key.SubKeyCount -eq 0
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
    }
}

function Format-Radio {
    param($Radio)
    $service = if ($Radio.Service) { $Radio.Service } else { 'none' }
    return "$($Radio.InstanceId): driver service '$service', problem $($Radio.Problem)"
}

function Test-StockRadioActive {
    param($Radio)
    return $null -ne $Radio -and $Radio.Service -eq $StockService -and $Radio.Problem -eq 0
}

function Add-RebootReason {
    param([string] $Reason)
    Write-Log "    restart required: $Reason"
    $script:RebootReasons.Add($Reason)
}

# SerCx2 publishes the port (a COM port interface) only when the controller starts with SerCxFriendlyName
# set; removing the value withdraws it only at the next controller restart.
function Test-UartPublished {
    return [DeckBtPnp]::HasEnabledInterface($UartId, [guid]$ComPortInterface)
}

# Restarts the UART controller so the change applies; if that fails, a Windows restart applies it.
function Restart-Uart {
    param([string] $Reason)
    $code = $null
    try {
        $code = Invoke-Native 'pnputil.exe' @('/restart-device', $UartId) -AllowedExitCodes @(0, 3010)
    }
    catch {
        Write-Log "    $($_.Exception.Message)"
    }
    if ($code -ne 0) { Add-RebootReason $Reason }
}

function Get-DenyEntries {
    $entries = @()
    if (Test-Path -LiteralPath $DenyListKey) {
        $key = Get-Item -LiteralPath $DenyListKey
        foreach ($name in $key.GetValueNames()) {
            $entries += [pscustomobject]@{ Name = $name; Data = [string]$key.GetValue($name) }
        }
    }
    return $entries
}

# Reconstructs, for a manual setup without an install record, the changes install.ps1 would have recorded.
function Get-DetectedChanges {
    $changes = @()
    # Same order as install.ps1 records them; they are reverted newest first.
    $entries = @(Get-DenyEntries)
    $ours = @($entries | Where-Object { $DenyIds -contains $_.Data })
    $radio = Get-RadioDevice
    if ($ours.Count -gt 0 -or ($null -ne $radio -and -not $radio.Service)) {
        $onlyOurs = $ours.Count -gt 0 -and $ours.Count -eq $entries.Count
        $changes += [pscustomobject]@{
            type                   = 'stock-transport'
            restrictionsKeyCreated = $onlyOurs
            denyListKeyCreated     = $onlyOurs
            denyDeviceIDsPrior     = $null
            retroactivePrior       = $null
            denyValues             = @($ours | ForEach-Object { [pscustomobject]@{ name = $_.Name; id = $_.Data; origin = 'detected' } })
            radioInstanceId        = if ($null -ne $radio) { $radio.InstanceId } else { $null }
            radioPriorService      = $StockService
            radioRemoved           = $true
            # No record of the prior policy values: Restore-StockTransport leaves a list others still use enabled.
            detected               = $true
        }
    }
    if ((Get-RegValue $UartParamsKey 'SerCxFriendlyName') -eq $UartFriendlyName) {
        $changes += [pscustomobject]@{ type = 'sercx-friendly-name'; priorValue = $null; origin = 'detected' }
    }
    if (Test-Path -LiteralPath $InstallDir) {
        $changes += [pscustomobject]@{ type = 'files'; path = $InstallDir; dirCreated = $true }
    }
    return $changes
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

    // For a file or an empty folder in use: MOVEFILE_DELAY_UNTIL_REBOOT deletes it at the next restart.
    public static void DeleteAtRestart(string path)
    {
        if (!MoveFileExW(path, null, 4)) throw new IOException("cannot schedule " + path + " for deletion: " + new Win32Exception().Message);
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

// Compiled with DeckBtDataDir; the script uses it only after Initialize-DataDir.
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

# ------------------------------------------------------------------ steps

function Remove-DeckBtService {
    $config = Get-CimInstance -ClassName Win32_Service -Filter "Name='$ServiceName'"
    if ($null -eq $config) {
        Write-Log "Service: $ServiceName not registered"
        return
    }
    Write-Log "Service: $ServiceName registered as $($config.PathName), state $($config.State)"
    $registeredExe = if ("$($config.PathName)" -match '^\s*"([^"]+)"') { $Matches[1] } else { ("$($config.PathName)" -split '\s+')[0] }
    # install.ps1 registers only $ServiceExe and refuses to replace another program's service.
    if ($registeredExe -ne $ServiceExe) {
        Write-Log "    it runs another program, not $ServiceExe; left as it is"
        return
    }
    $exe = if (Test-Path -LiteralPath $ServiceExe) { $ServiceExe } else { $null }
    $how = if ($null -ne $exe) { "'$exe uninstall' (falls back to sc.exe)" } else { 'sc.exe stop / sc.exe delete' }
    Invoke-Step 'remove-service' "Stop $ServiceName (detaches the radio) and delete it with $how" {
        $removed = $false
        if ($null -ne $exe) {
            try {
                Invoke-Native $exe @('uninstall') | Out-Null
                $removed = $true
            }
            catch {
                Write-Log "    $($_.Exception.Message); using sc.exe"
            }
        }
        if (-not $removed) {
            $service = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
            if ($null -ne $service -and $service.Status -ne 'Stopped') {
                Stop-Service -Name $ServiceName -Force
                (Get-Service -Name $ServiceName).WaitForStatus('Stopped', [TimeSpan]::FromSeconds(60))
            }
            # 1072: an earlier run already marked it for deletion; it goes at the next restart.
            $deleteCode = Invoke-Native 'sc.exe' @('delete', $ServiceName) -AllowedExitCodes @(0, 1072)
            if ($deleteCode -eq 1072) { Write-Log "    $ServiceName is already marked for deletion" }
        }
        # Wait for the process to exit so the UART and the program files are free; its stop watchdog allows 45 s.
        $deadline = (Get-Date).AddSeconds(60)
        while ((Get-Date) -lt $deadline -and @(Get-Process -Name 'deckbt-usbip' -ErrorAction SilentlyContinue).Count -gt 0) {
            Start-Sleep -Milliseconds 500
        }
        if ($null -ne (Get-Service -Name $ServiceName -ErrorAction SilentlyContinue)) {
            Add-RebootReason "$ServiceName is marked for deletion; a program still holds it open"
        }
    }
}

function Remove-Files {
    param($Change)
    if (-not (Test-Path -LiteralPath $InstallDir)) {
        Write-Log "Programs: $InstallDir already removed"
        return
    }
    Invoke-Step 'remove-files' "Delete $InstallDir; whatever is still in use is deleted at the next restart" {
        try {
            Remove-Item -LiteralPath $InstallDir -Recurse -Force
        }
        catch {
            # A program still running from the folder must not stop the UART and the stock radio being restored.
            Write-Log "    $($_.Exception.Message)"
            try {
                # Children before their folder: a folder is deleted at restart only once empty.
                foreach ($item in @(Get-ChildItem -LiteralPath $InstallDir -Recurse -Force | Sort-Object -Property { $_.FullName.Length } -Descending)) {
                    [DeckBtDataDir]::DeleteAtRestart($item.FullName)
                }
                [DeckBtDataDir]::DeleteAtRestart($InstallDir)
                Add-RebootReason "the rest of $InstallDir is deleted at the next restart"
            }
            catch {
                Write-Log "    $($_.Exception.Message)"
                Add-RebootReason "delete $InstallDir after the restart"
            }
        }
    }
}

function Restore-Uart {
    param($Change)
    $current = Get-RegValue $UartParamsKey 'SerCxFriendlyName'
    if ($current -ne $UartFriendlyName) {
        # A failed or interrupted controller restart after removing the value leaves the port published.
        if ($null -eq $current -and $null -eq $Change.priorValue -and (Test-UartPublished)) {
            Write-Log "UART: SerCxFriendlyName absent, but $UartId still publishes the port"
            Invoke-Step 'unpublish-uart' "Restart $UartId so SerCx2 withdraws the port; if that fails, restart Windows" {
                Restart-Uart "$UartId withdraws the published UART after a restart"
            }
            return
        }
        $currentText = if ($null -ne $current) { "'$current'" } else { 'absent' }
        Write-Log "UART: SerCxFriendlyName is $currentText, not set by DeckBtService; leaving it"
        return
    }
    if ($null -ne $Change.priorValue) {
        Invoke-Step 'restore-uart' "Restore $UartParamsKey SerCxFriendlyName = '$($Change.priorValue)' and restart $UartId; if that fails, restart Windows" {
            New-ItemProperty -LiteralPath $UartParamsKey -Name 'SerCxFriendlyName' -Value ([string]$Change.priorValue) -PropertyType String -Force | Out-Null
            Restart-Uart "$UartId applies the change after a restart"
        }
        return
    }
    Invoke-Step 'unpublish-uart' "Remove $UartParamsKey SerCxFriendlyName ('$UartFriendlyName') and restart $UartId; if that fails, restart Windows" {
        Remove-ItemProperty -LiteralPath $UartParamsKey -Name 'SerCxFriendlyName'
        Restart-Uart "$UartId withdraws the published UART after a restart"
    }
}

function Restore-StockTransport {
    param($Change)
    $entries = @(Get-DenyEntries)
    $listText = if ($entries.Count) { ($entries | ForEach-Object { "$($_.Name)=$($_.Data)" }) -join ', ' } else { 'none' }
    Write-Log "Deny policy: DenyDeviceIDs=$(Get-RegValue $PolicyKey 'DenyDeviceIDs'), DenyDeviceIDsRetroactive=$(Get-RegValue $PolicyKey 'DenyDeviceIDsRetroactive'), list: $listText"

    # Every value holding one of the radio's IDs goes, recorded or not: these IDs only ever mean this radio.
    $ours = @($entries | Where-Object { $DenyIds -contains $_.Data })
    $foreign = @($entries | Where-Object { $DenyIds -notcontains $_.Data })
    foreach ($value in @($Change.denyValues)) {
        if (@($ours | Where-Object { $_.Name -eq $value.name -and $_.Data -eq $value.id }).Count -eq 0) {
            Write-Log "    recorded deny entry '$($value.name)' = '$($value.id)' already gone"
        }
    }
    foreach ($entry in $ours) {
        Invoke-Step 'deny-policy-entry' "Remove $DenyListKey value '$($entry.Name)' = '$($entry.Data)'" {
            Remove-ItemProperty -LiteralPath $DenyListKey -Name $entry.Name
        }
    }
    $detected = $null -ne $Change.PSObject.Properties['detected']
    if ($detected -and $ours.Count -eq 0) {
        Write-Log '    no deny entries for ACPI\QCOM2066; policy left unchanged'
    }
    elseif ($detected -and $foreign.Count -gt 0) {
        Write-Log "    $($foreign.Count) other deny entries remain and no install record holds the prior DenyDeviceIDs value; it is left enabled"
    }
    else {
        if ($Change.denyListKeyCreated -and (Test-Path -LiteralPath $DenyListKey)) {
            Invoke-Step 'deny-policy-key' "Delete $DenyListKey if it is now empty" {
                if (Test-KeyEmpty $DenyListKey) { Remove-Item -LiteralPath $DenyListKey }
                else { Write-Log '    not empty; kept' }
            }
        }
        # The prior values return even when foreign entries remain: enabling DenyDeviceIDs enforced them too.
        if ($foreign.Count -gt 0) { Write-Log "    $($foreign.Count) deny entries not added by DeckBtService remain; DenyDeviceIDs returns to its value before install.ps1" }
        foreach ($pair in @(@('DenyDeviceIDs', $Change.denyDeviceIDsPrior), @('DenyDeviceIDsRetroactive', $Change.retroactivePrior))) {
            $valueName = $pair[0]
            $prior = $pair[1]
            $current = Get-RegValue $PolicyKey $valueName
            if ($null -eq $prior -and $null -ne $current) {
                Invoke-Step 'deny-policy-value' "Delete $PolicyKey $valueName (did not exist before)" {
                    Remove-ItemProperty -LiteralPath $PolicyKey -Name $valueName
                }
            }
            elseif ($null -ne $prior -and $current -ne $prior) {
                Invoke-Step 'deny-policy-value' "Restore $PolicyKey $valueName = $prior" {
                    New-ItemProperty -LiteralPath $PolicyKey -Name $valueName -Value ([int]$prior) -PropertyType DWord -Force | Out-Null
                }
            }
        }
        if ($Change.restrictionsKeyCreated -and (Test-Path -LiteralPath $PolicyKey)) {
            Invoke-Step 'deny-policy-key' "Delete $PolicyKey if it is now empty" {
                if (Test-KeyEmpty $PolicyKey) { Remove-Item -LiteralPath $PolicyKey }
                else { Write-Log '    not empty; kept' }
            }
        }
    }

    $radio = Get-RadioDevice
    if ($null -eq $radio) {
        Write-Log 'Stock radio: ACPI\QCOM2066 not found; nothing to reinstall'
        return
    }
    Write-Log "Stock radio: $(Format-Radio $radio)"
    if (Test-StockRadioActive $radio) { return }
    if (-not $Change.radioRemoved -and $radio.Service) {
        Write-Log "    driver '$($radio.Service)' was not changed by install.ps1; leaving it"
        return
    }
    $package = @(Get-ChildItem -LiteralPath $FirmwareRepository -Directory -Filter 'qcbtuart.inf_amd64_*' -ErrorAction SilentlyContinue |
        Sort-Object -Property LastWriteTime -Descending | Select-Object -First 1)
    Invoke-Step 'reinstall-stock-radio' "Remove devnode $($radio.InstanceId) and rescan so Windows installs the stock driver (qcbtuart.inf) again" {
        $codes = @()
        $codes += Invoke-Native 'pnputil.exe' @('/remove-device', $radio.InstanceId, '/subtree') -AllowedExitCodes @(0, 3010)
        Start-Sleep -Seconds 2
        $codes += Invoke-Native 'pnputil.exe' @('/scan-devices')
        Start-Sleep -Seconds 5
        $after = Get-RadioDevice
        if (-not (Test-StockRadioActive $after) -and $package.Count -gt 0) {
            $inf = Join-Path $package[0].FullName 'qcbtuart.inf'
            Write-Log "    not bound after the rescan; installing $inf on matching devices"
            # 259: no device needed the driver.
            $codes += Invoke-Native 'pnputil.exe' @('/add-driver', $inf, '/install') -AllowedExitCodes @(0, 259, 3010)
            Start-Sleep -Seconds 3
            $after = Get-RadioDevice
        }
        $afterText = if ($null -ne $after) { Format-Radio $after } else { 'not enumerated yet' }
        Write-Log "    after: $afterText"
        if ($codes -contains 3010 -or -not (Test-StockRadioActive $after)) {
            Add-RebootReason 'the Bluetooth radio returns to the stock driver after a restart'
        }
    }
}

function Remove-Usbip {
    param($Change)
    if (-not $Change.installedByUs) {
        Write-Log "usbip-win2: was already installed before install.ps1 ran (version $($Change.priorVersion)); leaving it"
        return
    }
    $uninstallString = Get-RegValue $UsbipUninstallKey 'UninstallString'
    if ($null -eq $uninstallString) {
        Write-Log 'usbip-win2: not installed'
        return
    }
    $uninstaller = if ($uninstallString -match '^\s*"([^"]+)"') { $Matches[1] } else { ($uninstallString -split '\s+')[0] }
    $setupLog = Join-Path $DataDir 'usbip-uninstall.log'
    Invoke-Step 'remove-usbip' "Run usbip-win2's uninstaller: $uninstaller /VERYSILENT /SUPPRESSMSGBOXES /NORESTART" {
        $process = Start-Process -FilePath $uninstaller -ArgumentList @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', "/LOG=`"$setupLog`"") -Wait -PassThru
        Write-Log "    uninstaller exit code $($process.ExitCode)"
        # The first uninstaller stage hands over to a copy of itself; wait for its registry entry to go.
        $deadline = (Get-Date).AddSeconds(180)
        while ((Get-Date) -lt $deadline -and (Test-Path -LiteralPath $UsbipUninstallKey)) { Start-Sleep -Seconds 1 }
        if (Test-Path -LiteralPath $UsbipUninstallKey) { throw "usbip-win2 is still registered after its uninstaller ran; see $setupLog" }
        Add-RebootReason 'usbip-win2 drivers are removed completely after a restart'
    }
}

# ------------------------------------------------------------------ main

try {
    # 32-bit PowerShell sees Program Files (x86) as $env:ProgramFiles and has no pnputil.exe.
    if (-not [Environment]::Is64BitProcess) { throw 'Run uninstall.ps1 from 64-bit Windows PowerShell, not Windows PowerShell (x86).' }
    if ($DryRun) { Write-Host 'DRY RUN: nothing will be changed.' }
    if (-not (Test-Admin)) {
        if (-not $DryRun) { throw 'Run uninstall.ps1 from an elevated PowerShell prompt (Run as administrator).' }
        Write-Host 'Note: not elevated; a real run requires administrator rights.'
    }
    $dataDirExists = Initialize-DataDir
    if (-not $DryRun -and $dataDirExists) {
        $script:LogFile = Join-Path $DataDir 'uninstall.log'
        Write-Log '=== uninstall.ps1'
    }

    $state = $null
    $changes = $null
    $noRecord = "No install record ($StatePath)"
    if (Test-Path -LiteralPath $StatePath) {
        try {
            # Read through the checked handle: a record others could write is ignored.
            $untrustedReason = $null
            $text = [DeckBtDataDir]::ReadTrusted($StatePath, [ref]$untrustedReason)
            if ($null -eq $text) {
                $noRecord = "Install record $StatePath ignored, $untrustedReason"
            }
            else {
                $state = $text | ConvertFrom-Json
                if ($null -eq $state -or $null -eq $state.PSObject.Properties['changes']) { throw 'it has no list of changes' }
                $changes = @($state.changes)
                Write-Log "Install record: $StatePath ($($changes.Count) recorded changes)"
            }
        }
        catch {
            $state = $null
            $noRecord = "Install record $StatePath is unreadable ($($_.Exception.GetBaseException().Message))"
        }
    }
    if ($null -eq $changes) {
        $changes = @(Get-DetectedChanges)
        Write-Log "${noRecord}: reverting the DeckBtService changes detected on this system instead ($($changes.Count) found)"
        foreach ($change in $changes) { Write-Log "    detected: $($change.type)" }
    }

    Remove-DeckBtService

    $usbipChange = $null
    for ($i = $changes.Count - 1; $i -ge 0; $i--) {
        $change = $changes[$i]
        switch ($change.type) {
            'service' { }
            'files' { Remove-Files $change }
            'sercx-friendly-name' { Restore-Uart $change }
            'stock-transport' { Restore-StockTransport $change }
            'usbip' { $usbipChange = $change }
            default { Write-Log "Unknown recorded change '$($change.type)'; skipped" }
        }
    }
    if (-not @($changes | Where-Object { $_.type -eq 'files' }) -and (Test-Path -LiteralPath $InstallDir)) {
        Remove-Files $null
    }

    $keepState = $false
    if ($RemoveUsbip) {
        if ($null -eq $usbipChange) {
            Write-Log 'usbip-win2: not installed by install.ps1; leaving it (remove it in Settings > Apps if you no longer need it)'
        }
        else {
            Remove-Usbip $usbipChange
        }
    }
    elseif ($null -ne $usbipChange -and $usbipChange.installedByUs) {
        Write-Log 'usbip-win2: kept (install.ps1 installed it; run uninstall.ps1 -RemoveUsbip to remove it)'
        $keepState = $true
    }

    # An unreadable or untrusted record goes too.
    if (Test-Path -LiteralPath $StatePath) {
        if ($keepState) {
            Invoke-Step 'install-record' "Rewrite $StatePath with only the usbip-win2 record, for a later -RemoveUsbip" {
                $state.changes = @($usbipChange)
                $state.updated = (Get-Date).ToString('o')
                Write-StateFile $state
            }
        }
        else {
            Invoke-Step 'install-record' "Delete $StatePath" {
                Remove-Item -LiteralPath $StatePath -Force
            }
        }
    }
    if (Test-Path -LiteralPath $StateTempPath) {
        Invoke-Step 'install-record' "Delete the leftover $StateTempPath" {
            Remove-Item -LiteralPath $StateTempPath -Force
        }
    }

    if ($RemoveLogs -and $dataDirExists) {
        Invoke-Step 'remove-logs' "Delete the DeckBtService logs in $DataDir ($($LogNames -join ', '), usbip-<version>-install.log) and the folder if nothing else remains" {
            $script:LogFile = $null
            # Securing the folder refused links; delete only the names DeckBtService writes.
            foreach ($file in @(Get-ChildItem -LiteralPath $DataDir -File -Force)) {
                if ($LogNames -contains $file.Name -or $file.Name -match '^usbip-[0-9.]+-install\.log$') {
                    Remove-Item -LiteralPath $file.FullName -Force
                }
            }
            $script:DataDirHandle.Dispose()
            $script:DataDirHandle = $null
            if (@(Get-ChildItem -LiteralPath $DataDir -Force).Count -eq 0) { Remove-Item -LiteralPath $DataDir -Force }
            else { Write-Host "    other files remain in $DataDir; folder kept" }
        }
    }
    elseif ($dataDirExists) {
        Write-Log "Logs kept in $DataDir (uninstall.ps1 -RemoveLogs deletes them)"
    }

    Write-Log ''
    if ($script:RebootReasons.Count -gt 0) {
        Write-Log "Restart Windows to finish: $($script:RebootReasons -join '; ')."
        exit 3010
    }
    if ($DryRun) { Write-Log 'Dry run complete.' } else { Write-Log 'DeckBtService is removed; Bluetooth runs on the stock driver.' }
    exit 0
}
catch {
    $message = $_.Exception.Message
    if ($script:LogFile) { Add-Content -LiteralPath $script:LogFile -Value "$(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') ERROR: $message" -Encoding UTF8 }
    Write-Host ''
    Write-Host "ERROR: $message" -ForegroundColor Red
    exit 1
}
