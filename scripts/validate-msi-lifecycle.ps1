<#
.SYNOPSIS
    Assert the installer lifecycle and ordinary-registration invariants.
.DESCRIPTION
    Reads built MSI databases through scripts/msi-query.ps1. It does not install
    packages or modify the databases.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string[]]$Path
)

$ErrorActionPreference = "Stop"
$queryTool = Join-Path $PSScriptRoot "msi-query.ps1"

function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

function Query-Msi([string]$Msi, [string]$Sql) {
    @(& $queryTool $Msi $Sql)
}

$contextClsid = "{BAE3934B-8A6A-4BFB-81BD-3FC599A1BAF1}"
$overlayClsid = "{BAE3934B-8A6A-4BFB-81BD-3FC599A1BAF2}"
$handlerName = " @nilesoft.shell"
$expectedRegistry = @(
    @{ Id = "ContextClassName"; Root = "0"; Key = "CLSID\$contextClsid"; Name = ""; Value = "Nilesoft.Shell" }
    @{ Id = "ContextServer"; Root = "0"; Key = "CLSID\$contextClsid\InprocServer32"; Name = ""; Value = '[$REGISTRATION]shell.dll' }
    @{ Id = "ContextThreadingModel"; Root = "0"; Key = "CLSID\$contextClsid\InprocServer32"; Name = "ThreadingModel"; Value = "Apartment" }
    @{ Id = "ApprovedContextHandler"; Root = "2"; Key = "SOFTWARE\Microsoft\Windows\CurrentVersion\Shell Extensions\Approved"; Name = $contextClsid; Value = $handlerName }
    @{ Id = "ContextHandlerFiles"; Root = "0"; Key = "*\shellex\ContextMenuHandlers\$handlerName"; Name = ""; Value = $contextClsid }
    @{ Id = "ContextHandlerDirectory"; Root = "0"; Key = "Directory\shellex\ContextMenuHandlers\$handlerName"; Name = ""; Value = $contextClsid }
    @{ Id = "ContextHandlerDrive"; Root = "0"; Key = "Drive\shellex\ContextMenuHandlers\$handlerName"; Name = ""; Value = $contextClsid }
    @{ Id = "ContextHandlerFolder"; Root = "0"; Key = "Folder\shellex\ContextMenuHandlers\$handlerName"; Name = ""; Value = $contextClsid }
    @{ Id = "ContextHandlerDirectoryBackground"; Root = "0"; Key = "Directory\Background\shellex\ContextMenuHandlers\$handlerName"; Name = ""; Value = $contextClsid }
    @{ Id = "ContextHandlerDesktopBackground"; Root = "0"; Key = "DesktopBackground\shellex\ContextMenuHandlers\$handlerName"; Name = ""; Value = $contextClsid }
    @{ Id = "ContextHandlerLibraryFolder"; Root = "0"; Key = "LibraryFolder\shellex\ContextMenuHandlers\$handlerName"; Name = ""; Value = $contextClsid }
    @{ Id = "ContextHandlerLibraryBackground"; Root = "0"; Key = "LibraryFolder\Background\shellex\ContextMenuHandlers\$handlerName"; Name = ""; Value = $contextClsid }
    @{ Id = "OverlayClassName"; Root = "0"; Key = "CLSID\$overlayClsid"; Name = ""; Value = "Nilesoft.Shell" }
    @{ Id = "OverlayServer"; Root = "0"; Key = "CLSID\$overlayClsid\InprocServer32"; Name = ""; Value = '[$REGISTRATION]shell.dll' }
    @{ Id = "OverlayThreadingModel"; Root = "0"; Key = "CLSID\$overlayClsid\InprocServer32"; Name = "ThreadingModel"; Value = "Apartment" }
    @{ Id = "OverlayIdentifier"; Root = "2"; Key = "SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer\ShellIconOverlayIdentifiers\$handlerName"; Name = ""; Value = $overlayClsid }
    @{ Id = "NssContentType"; Root = "0"; Key = ".nss"; Name = "Content Type"; Value = "text/plain" }
    @{ Id = "NssOpenCommand"; Root = "0"; Key = ".nss\shell\open\command"; Name = ""; Value = 'notepad "%1"' }
)

$expectedComponents = @{
    "x64"   = @{ Guid = "{0147E9FC-AA82-4EBE-A3AD-8D97CCD6E1A9}"; Attributes = "260" }
    "x86"   = @{ Guid = "{7F754C0B-5C79-4BD9-9FE5-F0B40ECEA830}"; Attributes = "4" }
    "arm64" = @{ Guid = "{FFBD0320-871D-4961-8146-8330E17F7DD6}"; Attributes = "260" }
}

foreach ($item in $Path) {
    $msi = (Resolve-Path -LiteralPath $item).Path
    $name = [System.IO.Path]::GetFileName($msi)
    $arch = if ($name -match "-(x64|x86|arm64)\.msi$") { $Matches[1] } else { $null }
    Assert-True ($null -ne $arch) "Cannot infer architecture from '$name'."

    $component = Query-Msi $msi 'SELECT `Component`, `ComponentId`, `Directory_`, `Attributes`, `KeyPath` FROM `Component` WHERE `Component` = ''REGISTRATION'''
    Assert-True ($component.Count -eq 1) "${name}: expected one REGISTRATION component."
    Assert-True ($component[0].ComponentId -eq $expectedComponents[$arch].Guid) "${name}: unexpected registration component GUID."
    Assert-True ($component[0].Attributes -eq $expectedComponents[$arch].Attributes) "${name}: unexpected registration component attributes."
    Assert-True ($component[0].Directory_ -eq "INSTALLFOLDER") "${name}: registration component is not rooted at INSTALLFOLDER."
    Assert-True ($component[0].KeyPath -eq "ContextClassName") "${name}: registration key path changed."

    $registry = Query-Msi $msi 'SELECT `Registry`, `Root`, `Key`, `Name`, `Value`, `Component_` FROM `Registry` WHERE `Component_` = ''REGISTRATION'''
    $actualRegistryIds = @($registry.Registry | Sort-Object)
    $expectedRegistryIds = @($expectedRegistry.Id | Sort-Object)
    Assert-True ($registry.Count -eq $expectedRegistry.Count -and
        @(Compare-Object $expectedRegistryIds $actualRegistryIds).Count -eq 0) "${name}: ordinary registration resource set changed."
    foreach ($expected in $expectedRegistry) {
        $row = @($registry | Where-Object Registry -eq $expected.Id)
        Assert-True ($row.Count -eq 1) "${name}: expected exactly one $($expected.Id) registry row."
        foreach ($field in @("Root", "Key", "Name", "Value")) {
            Assert-True ([string]$row[0].$field -ceq [string]$expected.$field) "${name}: $($expected.Id).$field changed."
        }
        Assert-True ($row[0].Component_ -eq "REGISTRATION") "${name}: $($expected.Id) moved to another component."
    }
    Assert-True (@($registry | Where-Object { $_.Registry -like "ContextHandler*" }).Count -eq 8) "${name}: expected exactly eight context-menu handlers."
    Assert-True (@($registry | Where-Object { $_.Key -like "*FolderExtensions*" }).Count -eq 0) "${name}: FolderExtensions must not be registered."
    Assert-True (@($registry | Where-Object { $_.Registry -in @("ContextServer", "OverlayServer") -and $_.Value -ne '[$REGISTRATION]shell.dll' }).Count -eq 0) "${name}: InprocServer32 no longer resolves from the registration component directory."

    $sequence = Query-Msi $msi 'SELECT `Action`, `Sequence`, `Condition` FROM `InstallExecuteSequence`'
    $initialize = @($sequence | Where-Object Action -eq "InstallInitialize")
    $removeExisting = @($sequence | Where-Object Action -eq "RemoveExistingProducts")
    $finalize = @($sequence | Where-Object Action -eq "InstallFinalize")
    $notify = @($sequence | Where-Object Action -eq "NotifyShellChanged")
    Assert-True ($initialize.Count -eq 1 -and $removeExisting.Count -eq 1) "${name}: upgrade sequence actions are missing or duplicated."
    Assert-True ([int]$removeExisting[0].Sequence -eq [int]$initialize[0].Sequence + 1) "${name}: RemoveExistingProducts is not immediately after InstallInitialize."
    Assert-True ($finalize.Count -eq 1 -and $notify.Count -eq 1 -and [int]$notify[0].Sequence -gt [int]$finalize[0].Sequence) "${name}: Shell notification must run after InstallFinalize."

    $customActions = Query-Msi $msi 'SELECT `Action`, `Type`, `Source`, `Target` FROM `CustomAction`'
    $forbiddenActions = @("OnUpdate", "OnUpdateElevatedData", "OnUpdateElevated", "OnRestoreConfigData", "OnRestoreConfig", "OnInstallData", "OnInstall", "OnInstallRollbackData", "OnInstallRollback", "OnUninstallData", "OnUninstall", "OnRestartExplorer")
    Assert-True (@($customActions | Where-Object { $_.Action -in $forbiddenActions }).Count -eq 0) "${name}: removed registration/rotation/restart custom action returned."
    Assert-True (@($customActions | Where-Object { $_.Target -match '(?i)shell\.exe|RestartExplorer|RotateOutOfTheWay|PruneRotations' }).Count -eq 0) "${name}: a custom action reintroduced executable registration, rotation, or Explorer control."
    Assert-True (@($customActions | Where-Object Action -eq "NotifyShellChanged").Count -eq 1) "${name}: NotifyShellChanged custom action is missing."

    $expectedActions = @{
        "BackupLegacyConfig" = "1"
        "RestoreLegacyConfigRollback" = "11521"
        "RestoreLegacyConfig" = "11265"
        "CleanupLegacyConfig" = "11841"
        "PrepareTreatAs" = "1"
        "TreatAsRollback" = "11521"
        "TreatAsApply" = "11265"
        "NotifyShellChanged" = "65"
    }
    foreach ($entry in $expectedActions.GetEnumerator()) {
        $row = @($customActions | Where-Object Action -eq $entry.Key)
        Assert-True ($row.Count -eq 1 -and $row[0].Type -eq $entry.Value) "${name}: $($entry.Key) custom-action type changed."
    }

    $expectedOrder = @{
        "BackupLegacyConfig" = 1001
        "PrepareTreatAs" = 1401
        "InstallInitialize" = 1500
        "RemoveExistingProducts" = 1501
        "RestoreLegacyConfigRollback" = 4001
        "RestoreLegacyConfig" = 4002
        "CleanupLegacyConfig" = 4003
        "TreatAsRollback" = 5001
        "TreatAsApply" = 5002
        "InstallFinalize" = 6600
        "NotifyShellChanged" = 6601
    }
    foreach ($entry in $expectedOrder.GetEnumerator()) {
        $row = @($sequence | Where-Object Action -eq $entry.Key)
        Assert-True ($row.Count -eq 1 -and [int]$row[0].Sequence -eq $entry.Value) "${name}: $($entry.Key) sequence changed."
    }

    $legacyCondition = '(NOT Installed) AND (NOT REMOVE) AND UPGRADEFOUND AND LEGACYCONFIGUPGRADE'
    $treatAsCondition = 'VersionNT64 AND (WindowsBuild >= 22000) AND (((NOT Installed) AND (NOT REMOVE)) OR ((REMOVE="ALL") AND (NOT UPGRADINGPRODUCTCODE)))'
    foreach ($action in @("BackupLegacyConfig", "RestoreLegacyConfigRollback", "RestoreLegacyConfig", "CleanupLegacyConfig")) {
        $row = @($sequence | Where-Object Action -eq $action)
        Assert-True ($row[0].Condition -ceq $legacyCondition) "${name}: $action condition no longer isolates pre-1.9.20 upgrades."
    }
    foreach ($action in @("PrepareTreatAs", "TreatAsRollback", "TreatAsApply")) {
        $row = @($sequence | Where-Object Action -eq $action)
        Assert-True ($row[0].Condition -ceq $treatAsCondition) "${name}: $action condition changed."
    }
    Assert-True (@($notify | Where-Object { [string]$_.Condition -ne "" }).Count -eq 0) "${name}: Shell notification unexpectedly became conditional."

    $upgrade = Query-Msi $msi 'SELECT `VersionMin`, `VersionMax`, `Attributes`, `ActionProperty` FROM `Upgrade`'
    $legacy = @($upgrade | Where-Object ActionProperty -eq "LEGACYCONFIGUPGRADE")
    Assert-True ($legacy.Count -eq 1 -and $legacy[0].VersionMin -eq "1.0.0" -and $legacy[0].VersionMax -eq "1.9.20" -and $legacy[0].Attributes -eq "258") "${name}: pre-1.9.20 config detector changed."

    $config = Query-Msi $msi 'SELECT `Component`, `Attributes` FROM `Component` WHERE `Component` = ''CONFIG'''
    Assert-True ($config.Count -eq 1 -and (([int]$config[0].Attributes -band 16) -eq 16)) "${name}: CONFIG is no longer permanent."

    $hidden = Query-Msi $msi 'SELECT `Property`, `Value` FROM `Property` WHERE `Property` = ''MsiHiddenProperties'''
    Assert-True ($hidden.Count -eq 1) "${name}: hidden custom-action data property is missing."
    foreach ($action in @("RestoreLegacyConfigRollback", "RestoreLegacyConfig", "CleanupLegacyConfig", "TreatAsRollback", "TreatAsApply")) {
        Assert-True ($action -in $hidden[0].Value.Split(';')) "${name}: $action data is not hidden from MSI logs."
    }

    $removeFiles = Query-Msi $msi 'SELECT `FileKey`, `InstallMode` FROM `RemoveFile`'
    foreach ($key in @("PurgeDllOld", "PurgeExeRotated", "PurgeExeOld")) {
        $row = @($removeFiles | Where-Object FileKey -eq $key)
        Assert-True ($row.Count -eq 1 -and $row[0].InstallMode -eq "3") "${name}: $key must run on install and uninstall."
    }

    Write-Output "ok $name ($arch)"
}
