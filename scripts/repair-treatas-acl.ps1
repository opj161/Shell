<#
.SYNOPSIS
    Repairs the ACL on the Windows 11 context-menu CLSID key.
.DESCRIPTION
    Shell registration used to fall back, on ERROR_ACCESS_DENIED, to taking
    ownership of

        HKLM\SOFTWARE\Classes\CLSID\{86ca1aa0-34aa-4e8b-a509-50c905bae2a2}

    and granting BUILTIN\Users GENERIC_ALL with inheritance - permanently, and
    inherited by the TreatAs subkey underneath it. That key decides which handler
    owns the Windows 11 Explorer context menu, so on an affected machine any
    unprivileged account can repoint InprocServer32 or TreatAs at a DLL of its
    own and have every other user's Explorer load it, administrators included.

    The registration code no longer does this - it borrows the minimum access it
    needs and puts the owner and DACL back - but a machine that has ever
    installed an affected build keeps the widened DACL until something restores
    it. This restores it:

      * BUILTIN\Users is narrowed to read, which is what a stock Windows CLSID
        key grants
      * ownership returns to NT SERVICE\TrustedInstaller
      * the TreatAs value is left alone, so Shell keeps working

    Requires elevation. Safe to run more than once.
.PARAMETER WhatIfOnly
    Report the current state and change nothing.
.EXAMPLE
    .\scripts\repair-treatas-acl.ps1 -WhatIfOnly
.EXAMPLE
    .\scripts\repair-treatas-acl.ps1
#>
[CmdletBinding()]
param([switch]$WhatIfOnly)

$ErrorActionPreference = 'Continue'
$key = 'SOFTWARE\Classes\CLSID\{86ca1aa0-34aa-4e8b-a509-50c905bae2a2}'
$path = "Registry::HKEY_LOCAL_MACHINE\$key"

function Show-State([string]$label) {
    $acl = Get-Acl $path
    Write-Output "$label"
    Write-Output "  owner: $($acl.Owner)"
    $acl.Access | Where-Object { $_.IdentityReference -eq 'BUILTIN\Users' } |
        ForEach-Object { "    Users: {0,-14} inherited={1} flags={2}" -f $_.RegistryRights, $_.IsInherited, $_.InheritanceFlags } |
        ForEach-Object { Write-Output $_ }
}

Show-State "BEFORE"

if ($WhatIfOnly) { return }

# --- 1. Narrow BUILTIN\Users back to read ---------------------------------
$acl = Get-Acl $path
$users = New-Object System.Security.Principal.SecurityIdentifier(
    [System.Security.Principal.WellKnownSidType]::BuiltinUsersSid, $null)

# PurgeAccessRules drops every explicit rule for this identity in one call; the
# per-ACE loop this replaces tripped over IdentityReference translation.
try {
    $acl.PurgeAccessRules($users)
    Write-Output "  purged explicit BUILTIN\Users ACEs"
}
catch {
    Write-Output "  purge failed: $($_.Exception.Message)"
    throw
}

$read = New-Object System.Security.AccessControl.RegistryAccessRule(
    $users,
    [System.Security.AccessControl.RegistryRights]::ReadKey,
    [System.Security.AccessControl.InheritanceFlags]'ContainerInherit',
    [System.Security.AccessControl.PropagationFlags]::None,
    [System.Security.AccessControl.AccessControlType]::Allow)
$acl.AddAccessRule($read)

Set-Acl -Path $path -AclObject $acl
Write-Output "  DACL updated"

# --- 2. Hand ownership back to TrustedInstaller ---------------------------
# Needs SeRestorePrivilege: an administrator can take ownership, but giving it
# to a principal they are not a member of is a restore operation.
$sig = @'
using System;
using System.Runtime.InteropServices;
public static class Priv {
    [StructLayout(LayoutKind.Sequential)] public struct LUID { public uint Low; public int High; }
    [StructLayout(LayoutKind.Sequential)] public struct TOKEN_PRIVILEGES {
        public uint Count; public LUID Luid; public uint Attr; }
    [DllImport("advapi32.dll", SetLastError=true)] public static extern bool OpenProcessToken(IntPtr h, uint acc, out IntPtr tok);
    [DllImport("advapi32.dll", SetLastError=true)] public static extern bool LookupPrivilegeValue(string sys, string name, out LUID luid);
    [DllImport("advapi32.dll", SetLastError=true)] public static extern bool AdjustTokenPrivileges(IntPtr tok, bool dis, ref TOKEN_PRIVILEGES np, uint len, IntPtr prev, IntPtr ret);
    [DllImport("kernel32.dll")] public static extern IntPtr GetCurrentProcess();
    public static bool Enable(string name) {
        IntPtr tok;
        if (!OpenProcessToken(GetCurrentProcess(), 0x28, out tok)) return false;
        LUID luid;
        if (!LookupPrivilegeValue(null, name, out luid)) return false;
        TOKEN_PRIVILEGES tp = new TOKEN_PRIVILEGES();
        tp.Count = 1; tp.Luid = luid; tp.Attr = 0x2;
        return AdjustTokenPrivileges(tok, false, ref tp, 0, IntPtr.Zero, IntPtr.Zero);
    }
}
'@
Add-Type -TypeDefinition $sig -ErrorAction SilentlyContinue | Out-Null

$okRestore = [Priv]::Enable('SeRestorePrivilege')
$okTake = [Priv]::Enable('SeTakeOwnershipPrivilege')
Write-Output "  SeRestorePrivilege=$okRestore SeTakeOwnershipPrivilege=$okTake"

try {
    $acl = Get-Acl $path
    $ti = New-Object System.Security.Principal.NTAccount('NT SERVICE\TrustedInstaller')
    $acl.SetOwner($ti)
    Set-Acl -Path $path -AclObject $acl
    Write-Output "  owner restored to NT SERVICE\TrustedInstaller"
}
catch {
    Write-Output "  owner NOT restored: $($_.Exception.Message)"
    Write-Output "  (the DACL fix above is the part that matters; owner stays BUILTIN\Administrators)"
}

Show-State "AFTER"

# --- 3. And the subkey that inherited it ----------------------------------
foreach ($sub in @('TreatAs', 'InProcServer32')) {
    $p = "$path\$sub"
    if (Test-Path $p) {
        $a = Get-Acl $p
        $bad = $a.Access | Where-Object {
            $_.IdentityReference -eq 'BUILTIN\Users' -and
            ($_.RegistryRights -band [System.Security.AccessControl.RegistryRights]::SetValue)
        }
        Write-Output "  $sub : Users write access = $(if ($bad) { 'STILL PRESENT' } else { 'gone' })"
    }
}

# The redirect must survive all of this.
$treat = (Get-ItemProperty "$path\TreatAs" -Name '(default)' -ErrorAction SilentlyContinue).'(default)'
Write-Output "  TreatAs value still: $treat"
