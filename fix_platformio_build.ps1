# ESP32 WiFi-Repeater PlatformIO Build-Fehler Behebung
# Ausführung: powershell -ExecutionPolicy Bypass -File fix_platformio_build.ps1

param(
    [switch]$SkipCleanCache = $false,
    [switch]$VerboseOutput = $false
)

# Farben für Output
$colors = @{
    'Success' = 'Green'
    'Error'   = 'Red'
    'Info'    = 'Cyan'
    'Warning' = 'Yellow'
}

function Write-Status {
    param([string]$Message, [string]$Status = 'Info')
    $timestamp = Get-Date -Format "HH:mm:ss"
    Write-Host "[$timestamp] " -NoNewline
    Write-Host $Message -ForegroundColor $colors[$Status]
}

function Confirm-AdminRights {
    $admin = ([Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    if (-not $admin) {
        Write-Status "Fehler: Administratorrechte erforderlich" 'Error'
        Write-Host "Skript mit Admin-Rechten erneut ausführen"
        exit 1
    }
}

function Remove-PlatformIOCache {
    param([string]$Path)
    
    if (Test-Path $Path) {
        Write-Status "Lösche: $Path" 'Info'
        try {
            Remove-Item -Path $Path -Recurse -Force -ErrorAction Stop
            Write-Status "✓ Erfolgreich gelöscht" 'Success'
            return $true
        } catch {
            Write-Status "✗ Fehler beim Löschen: $_" 'Error'
            return $false
        }
    } else {
        Write-Status "- Nicht gefunden: $Path" 'Info'
        return $true
    }
}

function Execute-Command {
    param(
        [string]$Command,
        [string]$Description,
        [string]$WorkingDir = $null
    )
    
    Write-Status "Ausführung: $Description" 'Info'
    
    $originalDir = Get-Location
    if ($WorkingDir) {
        Set-Location $WorkingDir
    }
    
    try {
        if ($VerboseOutput) {
            & cmd /c $Command
        } else {
            & cmd /c "$Command 2>&1" | Out-Null
        }
        
        if ($LASTEXITCODE -eq 0) {
            Write-Status "✓ $Description erfolgreich" 'Success'
            return $true
        } else {
            Write-Status "✗ $Description fehlgeschlagen (Code: $LASTEXITCODE)" 'Error'
            return $false
        }
    } catch {
        Write-Status "✗ Fehler: $_" 'Error'
        return $false
    } finally {
        Set-Location $originalDir
    }
}

# ============================================================================
# HAUPTPROGRAMM
# ============================================================================

Write-Host ""
Write-Host "╔══════════════════════════════════════════════════════════════════╗" -ForegroundColor Cyan
Write-Host "║        ESP32 WiFi-Repeater PlatformIO Build-Fehler Fixer        ║" -ForegroundColor Cyan
Write-Host "╚══════════════════════════════════════════════════════════════════╝" -ForegroundColor Cyan
Write-Host ""

# Benutzerprofilpfade
$platformioPkgPath = "$env:USERPROFILE\.platformio\packages"
$platformioCachePath = "$env:USERPROFILE\.platformio\cache"
$platformioEnvPath = "$env:USERPROFILE\.platformio\penv"

Write-Status "Benutzerprofil: $env:USERPROFILE" 'Info'

# Schritt 1: Admin-Rechte
Write-Status "Überprüfe Administratorrechte..." 'Info'
Confirm-AdminRights

# Schritt 2: PlatformIO Cache löschen
Write-Host ""
Write-Status "=== Schritt 1: Cache-Bereinigung ===" 'Info'

$cacheDeleted = $true
if (-not $SkipCleanCache) {
    $cacheDeleted = Remove-PlatformIOCache -Path $platformioPkgPath
    $cacheDeleted = $cacheDeleted -and (Remove-PlatformIOCache -Path $platformioCachePath)
} else {
    Write-Status "Cache-Löschung übersprungen" 'Warning'
}

if (-not $cacheDeleted) {
    Write-Status "Fehler beim Cache-Löschen - möglicherweise sind Prozesse noch aktiv" 'Warning'
    Write-Host "  → PlatformIO IDE/VS Code schließen und erneut versuchen"
}

# Schritt 3: PlatformIO Core Update
Write-Host ""
Write-Status "=== Schritt 2: PlatformIO Core Update ===" 'Info'
Execute-Command "platformio core update" "PlatformIO Core Update" | Out-Null

# Schritt 4: Projekt-Verzeichnis prüfen
Write-Host ""
Write-Status "=== Schritt 3: Projekt-Verifikation ===" 'Info'

$projectFiles = @('platformio.ini', 'CMakeLists.txt', 'sdkconfig.defaults', 'src\main.c')
$allFilesExist = $true

foreach ($file in $projectFiles) {
    if (Test-Path $file) {
        Write-Status "✓ Gefunden: $file" 'Success'
    } else {
        Write-Status "✗ Fehlend: $file" 'Error'
        $allFilesExist = $false
    }
}

if (-not $allFilesExist) {
    Write-Status "Kritischer Fehler: Erforderliche Projektdateien fehlen" 'Error'
    exit 1
}

# Schritt 5: Projekt bereinigen
Write-Host ""
Write-Status "=== Schritt 4: Projekt-Bereinigung ===" 'Info'
Execute-Command "platformio run -t clean" "Projekt Clean"

# Schritt 6: Build mit Verbose-Output (optional)
Write-Host ""
Write-Status "=== Schritt 5: Build-Prozess ===" 'Info'

if ($VerboseOutput) {
    Write-Status "Führe Build mit ausführlichem Output aus..." 'Info'
    Execute-Command "platformio run -v" "Verbose Build"
} else {
    Write-Status "Führe Build aus (standard output)..." 'Info'
    Execute-Command "platformio run" "Build"
}

# Schritt 7: Upload
Write-Host ""
Write-Status "=== Schritt 6: Upload auf ESP32 ===" 'Info'

if ($VerboseOutput) {
    Execute-Command "platformio run --target upload -v" "Upload (verbose)"
} else {
    Execute-Command "platformio run --target upload" "Upload"
}

# Abschlussmeldung
Write-Host ""
Write-Host "╔══════════════════════════════════════════════════════════════════╗" -ForegroundColor Cyan
Write-Status "Build-Prozess abgeschlossen" 'Info'
Write-Host "╚══════════════════════════════════════════════════════════════════╝" -ForegroundColor Cyan
Write-Host ""

if ($VerboseOutput) {
    Write-Host "Verbose-Modus aktiv - siehe Output oben für Details"
} else {
    Write-Host "Tipp: Für ausführlichen Output erneut ausführen mit:"
    Write-Host "  powershell -ExecutionPolicy Bypass -File fix_platformio_build.ps1 -VerboseOutput"
}

Write-Host ""
