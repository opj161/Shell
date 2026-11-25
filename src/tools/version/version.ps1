# Version patching for Shell. Run through version.cmd.
#
# The version fields are rewritten where they stand in the live files. This tool
# used to copy a whole-file template over each target, so every edit to
# Resource.h, the manifest or Shell.def had to be mirrored into this folder by
# hand: Shell.def's template lost the ShellCheckConfig export that way, and an
# earlier template over var.wxi deleted the package identity. A pattern that is
# no longer found is an error, never a silent skip.

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$ScriptPath = $PSCommandPath
$ScriptDir = Split-Path -Path $ScriptPath -Parent

$ChangeBuild	= 0
$ChangeRevis	= 0

$___Build__	= 20	# auto increase
$___Revis__	= 0	# auto increase

$Major	= 1
$Minor	= 9
$Build	= $___Build__
$Revis	= $___Revis__
$YEAR	= [int](Get-Date -Format yyyy)

if($ChangeBuild) { $Build = $Build + 1 }
if($ChangeRevis) { $Revis = $Revis + 1 }

$Version = "$Major.$Minor.$Build.$Revis"
Write-Host "Nilesoft Shell version number: $Version"

# Each edit is a pattern whose group 1 is kept and whose remainder becomes the
# value. The files are ASCII, so reading and writing them as UTF-8 without a BOM
# preserves every byte that is not a version field, line endings included.
function Set-VersionFields([string]$relative, [object[]]$edits)
{
	$path = Join-Path $ScriptDir $relative
	$text = [IO.File]::ReadAllText($path)
	$out = $text
	foreach($edit in $edits)
	{
		$regex = [regex]::new($edit[0], 'Multiline')
		if(-not $regex.IsMatch($out))
		{
			throw "version.ps1: '$($edit[0])' matches nothing in $relative"
		}
		$out = $regex.Replace($out, '${1}' + $edit[1], 1)
	}
	if($out -ne $text)
	{
		[IO.File]::WriteAllText($path, $out, [Text.UTF8Encoding]::new($false))
		Write-Host "  updated $relative"
	}
}

# Stable installer identity is not generated data; it must survive this tool.
$identityFile = Join-Path $ScriptDir '..\..\setup\wix\identity.wxi'
$identityHash = (Get-FileHash -LiteralPath $identityFile -Algorithm SHA256).Hash

Set-VersionFields '..\..\shared\Resource.h' @(
	, @('^(#define\s+VERSION_MAJOR\s+)\d+', $Major)
	, @('^(#define\s+VERSION_MINOR\s+)\d+', $Minor)
	, @('^(#define\s+VERSION_BUILD\s+)\d+', $Build)
	, @('^(#define\s+VERSION_REV\s+)\d+', $Revis)
	, @('^(#define\s+VERSION_YEAR\s+)\d+', $YEAR)
	, @('^(#define\s+APP_PRODUCT_VERSION\s+L")[^"]*', "$Major.$Minor")
	, @('^(#define\s+APP_VERSION\s+L")[^"]*', "$Major.$Minor.$Build")
)
Set-VersionFields '..\..\shared\Resource\manifest.xml' @(
	, @('(<assemblyIdentity\s+version=")[^"]*', $Version)
)
Set-VersionFields '..\..\dll\src\Shell.def' @(
	, @('^(VERSION\s+)\d+\.\d+', "$Major.$Minor")
)
Set-VersionFields '..\..\setup\wix\version.wxi' @(
	, @("^(<\?define Version = ')[^']*", "$Major.$Minor.$Build")
)

if((Get-FileHash -LiteralPath $identityFile -Algorithm SHA256).Hash -ne $identityHash)
{
	throw "version.ps1 modified $identityFile. Package identity is not generated data."
}

# Persist an auto-increased counter in this script, and only then.
if($ChangeBuild -or $ChangeRevis)
{
	$self = [IO.File]::ReadAllText($ScriptPath)
	if($ChangeBuild) { $self = [regex]::Replace($self, '(\$___Build__\s*=\s*)\d+', '${1}' + $Build) }
	if($ChangeRevis) { $self = [regex]::Replace($self, '(\$___Revis__\s*=\s*)\d+', '${1}' + $Revis) }
	[IO.File]::WriteAllText($ScriptPath, $self, [Text.UTF8Encoding]::new($false))
}
