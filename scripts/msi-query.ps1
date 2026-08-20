<#
.SYNOPSIS
    Read-only SQL query against a built MSI, for inspecting what WiX actually
    emitted rather than what the authoring appears to say.
.DESCRIPTION
    Windows Installer decides component identity, sequencing and file removal
    from the tables in the package, and those are the part that keeps being
    surprising. Nothing here writes: the database is opened read-only (mode 0).

    https://learn.microsoft.com/en-us/windows/win32/msi/installer-opendatabase
    https://learn.microsoft.com/en-us/windows/win32/msi/sql-syntax
.PARAMETER Path
    The .msi to read.
.PARAMETER Query
    A SQL SELECT. Defaults to the Property table.
.EXAMPLE
    .\scripts\msi-query.ps1 src\bin\setup-x64.msi "SELECT Component, ComponentId, Attributes FROM Component"
.EXAMPLE
    .\scripts\msi-query.ps1 src\bin\setup-x64.msi "SELECT Action, Sequence, Condition FROM InstallExecuteSequence"
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string]$Path,

    [Parameter(Position = 1)]
    [string]$Query = "SELECT Property, Value FROM Property"
)

$ErrorActionPreference = "Stop"

$full = (Resolve-Path -LiteralPath $Path).Path
$installer = New-Object -ComObject WindowsInstaller.Installer

# msiOpenDatabaseModeReadOnly = 0. Anything else can rewrite the package being
# inspected, which is never what a query wants.
$database = $installer.GetType().InvokeMember(
    "OpenDatabase", "InvokeMethod", $null, $installer, @($full, 0))

try {
    $view = $database.GetType().InvokeMember(
        "OpenView", "InvokeMethod", $null, $database, @($Query))
    $view.GetType().InvokeMember("Execute", "InvokeMethod", $null, $view, $null) | Out-Null

    # Column names come from the view, so callers do not have to restate them.
    $info = $view.GetType().InvokeMember("ColumnInfo", "GetProperty", $null, $view, @(0))
    $count = [int]$info.GetType().InvokeMember("FieldCount", "GetProperty", $null, $info, $null)
    $names = @(1..$count | ForEach-Object {
        $info.GetType().InvokeMember("StringData", "GetProperty", $null, $info, @([int]$_))
    })

    while ($true) {
        $record = $view.GetType().InvokeMember("Fetch", "InvokeMethod", $null, $view, $null)
        if ($null -eq $record) { break }

        $row = [ordered]@{}
        for ($i = 1; $i -le $count; $i++) {
            $row[$names[$i - 1]] = $record.GetType().InvokeMember(
                "StringData", "GetProperty", $null, $record, @([int]$i))
        }
        [pscustomobject]$row
    }

    $view.GetType().InvokeMember("Close", "InvokeMethod", $null, $view, $null) | Out-Null
}
finally {
    [void][Runtime.InteropServices.Marshal]::ReleaseComObject($database)
    [void][Runtime.InteropServices.Marshal]::ReleaseComObject($installer)
}
