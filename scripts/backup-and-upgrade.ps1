# Nilesoft Shell Backup and Upgrade Script

$ErrorActionPreference = "Stop"

$installDir = "C:\Program Files\Nilesoft Shell"
$backupDir = "c:\Users\j_opp\Projects\Shell\backup_installed_shell"
$sourceBinDir = "c:\Users\j_opp\Projects\Shell\src\bin"

Write-Host "=========================================" -ForegroundColor Cyan
Write-Host " 1. Backing up existing installation..." -ForegroundColor Cyan
Write-Host "=========================================" -ForegroundColor Cyan

if (Test-Path -Path $installDir) {
    if (-not (Test-Path -Path $backupDir)) {
        New-Item -ItemType Directory -Path $backupDir -Force | Out-Null
    }
    Copy-Item -Path "$installDir\*" -Destination $backupDir -Recurse -Force
    Write-Host "[OK] Existing installation backed up to: $backupDir" -ForegroundColor Green
} else {
    Write-Host "[INFO] No existing installation found at $installDir." -ForegroundColor Yellow
}

Write-Host "`n=========================================" -ForegroundColor Cyan
Write-Host " 2. Stopping / Unregistering old Shell..." -ForegroundColor Cyan
Write-Host "=========================================" -ForegroundColor Cyan

try {
    if (Test-Path "$installDir\shell.exe") {
        & "$installDir\shell.exe" -unregister
        Write-Host "[OK] Unregistered old shell.dll" -ForegroundColor Green
    }
} catch {
    Write-Host "[WARN] Unregister error (ignoring): $_" -ForegroundColor Yellow
}

# Stop explorer to release DLL file handles cleanly
Write-Host "[INFO] Stopping Windows Explorer to release file locks..." -ForegroundColor Cyan
Get-Process -Name explorer -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 2

Write-Host "`n=========================================" -ForegroundColor Cyan
Write-Host " 3. Deploying updated binaries..." -ForegroundColor Cyan
Write-Host "=========================================" -ForegroundColor Cyan

# Preserve user custom shell.nss by checking if it exists
$userNssPath = "$backupDir\shell.nss"
$hasCustomNss = Test-Path $userNssPath

# If shell.dll is still locked by any remaining process, rotate it to shell.dll.old
if (Test-Path "$installDir\shell.dll") {
    try {
        Remove-Item "$installDir\shell.dll.old" -Force -ErrorAction SilentlyContinue
        Move-Item -Path "$installDir\shell.dll" -Destination "$installDir\shell.dll.old" -Force -ErrorAction SilentlyContinue
    } catch {}
}

# Copy updated shell.dll, shell.exe, and license
Copy-Item -Path "$sourceBinDir\shell.dll" -Destination "$installDir\shell.dll" -Force
Copy-Item -Path "$sourceBinDir\shell.exe" -Destination "$installDir\shell.exe" -Force
Copy-Item -Path "$sourceBinDir\license.txt" -Destination "$installDir\license.txt" -Force

# Deploy full imports folder including all 15 language definitions
if (-not (Test-Path "$installDir\imports\lang")) {
    New-Item -ItemType Directory -Path "$installDir\imports\lang" -Force | Out-Null
}
Copy-Item -Path "$sourceBinDir\imports\*" -Destination "$installDir\imports" -Recurse -Force

# Update shell.nss while merging custom items if needed
if ($hasCustomNss) {
    $customContent = Get-Content $userNssPath -Raw
    # If the user has custom items like Repomix, preserve them
    if ($customContent -match "item\(title='Repomix'") {
        Write-Host "[INFO] Preserving custom Repomix menu item in shell.nss" -ForegroundColor Cyan
        $newContent = Get-Content "$sourceBinDir\shell.nss" -Raw
        # Insert the custom item before terminal.nss
        $mergedContent = $newContent -replace "(import 'imports/terminal\.nss')", "`nitem(title='Repomix' `n`timage='C:\.bin\repomix-logo.ico' `n`tcmd='pwsh.exe' `n`targs='-Command `"rp`"' `n`tdir=@sel.path `n`ttype='back|dir')`n`n`$1"
        Set-Content -Path "$installDir\shell.nss" -Value $mergedContent -Encoding UTF8
    } else {
        # Copy user's shell.nss directly
        Copy-Item -Path $userNssPath -Destination "$installDir\shell.nss" -Force
    }
} else {
    Copy-Item -Path "$sourceBinDir\shell.nss" -Destination "$installDir\shell.nss" -Force
}

Write-Host "[OK] Binaries and configurations deployed to $installDir" -ForegroundColor Green

Write-Host "`n=========================================" -ForegroundColor Cyan
Write-Host " 4. Registering updated shell.dll..." -ForegroundColor Cyan
Write-Host "=========================================" -ForegroundColor Cyan

& "$installDir\shell.exe" -register
Write-Host "[OK] Registered new shell.dll successfully" -ForegroundColor Green

Write-Host "`n=========================================" -ForegroundColor Cyan
Write-Host " 5. Restarting Explorer..." -ForegroundColor Cyan
Write-Host "=========================================" -ForegroundColor Cyan

Start-Process "explorer.exe"
Start-Sleep -Seconds 2
Write-Host "[OK] Windows Explorer restarted" -ForegroundColor Green

Write-Host "`nUpgrade and Installation Complete!" -ForegroundColor Green
