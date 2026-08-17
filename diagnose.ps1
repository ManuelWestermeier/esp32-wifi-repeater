# Diagnose-Skript für ESP32 PlatformIO Build-Probleme
# powershell -ExecutionPolicy Bypass -File diagnose.ps1

Write-Host "`n╔════════════════════════════════════════════════════════╗" -ForegroundColor Cyan
Write-Host "║   ESP32 PlatformIO Build-Problem Diagnose              ║" -ForegroundColor Cyan
Write-Host "╚════════════════════════════════════════════════════════╝`n" -ForegroundColor Cyan

$userProfile = $env:USERPROFILE
$results = @{}

# 1. PlatformIO Installation
Write-Host "1️⃣  PlatformIO Installation" -ForegroundColor Yellow
$pioVersion = & cmd /c "platformio --version" 2>&1
if ($LASTEXITCODE -eq 0) {
    Write-Host "   ✅ PlatformIO installiert: $pioVersion" -ForegroundColor Green
    $results['pio_installed'] = $true
} else {
    Write-Host "   ❌ PlatformIO nicht gefunden (PATH-Problem?)" -ForegroundColor Red
    $results['pio_installed'] = $false
}

# 2. Python verfügbar
Write-Host "`n2️⃣  Python Abhängigkeit" -ForegroundColor Yellow
$pythonVersion = & cmd /c "python --version" 2>&1
if ($LASTEXITCODE -eq 0) {
    Write-Host "   ✅ Python verfügbar: $pythonVersion" -ForegroundColor Green
    $results['python_ok'] = $true
} else {
    Write-Host "   ⚠️  Python nicht im PATH (wird von PlatformIO bereitgestellt)" -ForegroundColor Yellow
    $results['python_ok'] = $false
}

# 3. Projekt-Struktur
Write-Host "`n3️⃣  Projekt-Struktur" -ForegroundColor Yellow
$requiredFiles = @(
    'platformio.ini',
    'CMakeLists.txt',
    'sdkconfig.defaults',
    'src\main.c',
    'src\CMakeLists.txt'
)

$allFilesPresent = $true
foreach ($file in $requiredFiles) {
    $exists = Test-Path $file
    $status = if ($exists) { "✅" } else { "❌" }
    Write-Host "   $status $file" -ForegroundColor $(if ($exists) { 'Green' } else { 'Red' })
    $allFilesPresent = $allFilesPresent -and $exists
}
$results['project_ok'] = $allFilesPresent

# 4. PlatformIO Cache Status
Write-Host "`n4️⃣  PlatformIO Cache & Packages" -ForegroundColor Yellow

$cachePath = "$userProfile\.platformio\cache"
$packagesPath = "$userProfile\.platformio\packages"
$envPath = "$userProfile\.platformio\penv"

if (Test-Path $cachePath) {
    $cacheSize = (Get-ChildItem $cachePath -Recurse | Measure-Object -Property Length -Sum).Sum / 1MB
    Write-Host "   📁 Cache: $([Math]::Round($cacheSize, 2)) MB" -ForegroundColor Cyan
} else {
    Write-Host "   📁 Cache: Nicht vorhanden (wird bei nächstem Build erstellt)" -ForegroundColor Cyan
}

if (Test-Path $packagesPath) {
    $pkgCount = (Get-ChildItem $packagesPath -Directory).Count
    Write-Host "   📦 Packages: $pkgCount Pakete gefunden" -ForegroundColor Cyan
    
    # Framework prüfen
    $espIdfPath = Get-ChildItem $packagesPath -Filter "framework-espidf*" -Directory | Select-Object -First 1
    if ($espIdfPath) {
        Write-Host "   ✅ ESP-IDF Framework: Installiert ($($espIdfPath.Name))" -ForegroundColor Green
    } else {
        Write-Host "   ❌ ESP-IDF Framework: FEHLT!" -ForegroundColor Red
    }
} else {
    Write-Host "   📦 Packages: Nicht vorhanden (werden heruntergeladen)" -ForegroundColor Yellow
}

# 5. COM-Ports (USB)
Write-Host "`n5️⃣  USB/COM-Ports" -ForegroundColor Yellow
$comPorts = Get-WmiObject Win32_SerialPort
if ($comPorts) {
    foreach ($port in $comPorts) {
        Write-Host "   ✅ $($port.Name): $($port.Description)" -ForegroundColor Green
    }
} else {
    Write-Host "   ⚠️  Keine COM-Ports gefunden" -ForegroundColor Yellow
    Write-Host "   → ESP32 nicht verbunden oder Treiber fehlt" -ForegroundColor Yellow
}

# 6. platformio.ini Validierung
Write-Host "`n6️⃣  platformio.ini Konfiguration" -ForegroundColor Yellow
if (Test-Path 'platformio.ini') {
    $iniContent = Get-Content 'platformio.ini'
    
    $hasPlatform = $iniContent | Where-Object { $_ -match 'platform\s*=' }
    $hasFramework = $iniContent | Where-Object { $_ -match 'framework\s*=' }
    $hasBoard = $iniContent | Where-Object { $_ -match 'board\s*=' }
    
    Write-Host "   ✅ platform: $(if ($hasPlatform) { 'OK' } else { 'FEHLT' })" -ForegroundColor $(if ($hasPlatform) { 'Green' } else { 'Red' })
    Write-Host "   ✅ framework: $(if ($hasFramework) { 'OK' } else { 'FEHLT' })" -ForegroundColor $(if ($hasFramework) { 'Green' } else { 'Red' })
    Write-Host "   ✅ board: $(if ($hasBoard) { 'OK' } else { 'FEHLT' })" -ForegroundColor $(if ($hasBoard) { 'Green' } else { 'Red' })
}

# 7. Disk-Speicher
Write-Host "`n7️⃣  System-Ressourcen" -ForegroundColor Yellow
$disk = Get-PSDrive C
$diskUsagePct = [Math]::Round(($disk.Used / $disk.Total) * 100, 1)
$diskFreeMB = [Math]::Round($disk.Free / 1MB, 0)

Write-Host "   💾 Speicher frei: $diskFreeMB MB ($($100 - $diskUsagePct)%)" -ForegroundColor $(if ($diskFreeMB -gt 1000) { 'Green' } else { 'Red' })

$ram = Get-WmiObject Win32_OperatingSystem
$ramUsedPct = [Math]::Round(($ram.TotalVisibleMemorySize - $ram.FreePhysicalMemory) / $ram.TotalVisibleMemorySize * 100, 1)
Write-Host "   🧠 RAM verfügbar: $ramUsedPct% genutzt" -ForegroundColor $(if ($ramUsedPct -lt 80) { 'Green' } else { 'Yellow' })

# Empfehlungen
Write-Host "`n╔════════════════════════════════════════════════════════╗" -ForegroundColor Cyan
Write-Host "║   EMPFEHLUNGEN                                         ║" -ForegroundColor Cyan
Write-Host "╚════════════════════════════════════════════════════════╝`n" -ForegroundColor Cyan

if (-not $results['pio_installed']) {
    Write-Host "⚠️  PlatformIO nicht installiert" -ForegroundColor Red
    Write-Host "   Installieren mit: pip install platformio`n" -ForegroundColor Yellow
}

if (-not $results['project_ok']) {
    Write-Host "⚠️  Projektdateien fehlen" -ForegroundColor Red
    Write-Host "   Stellen Sie sicher, dass alle erforderlichen Dateien vorhanden sind`n" -ForegroundColor Yellow
}

if (-not $comPorts) {
    Write-Host "⚠️  Keine USB-COM-Ports erkannt" -ForegroundColor Red
    Write-Host "   → ESP32 verbinden oder Treiber installieren:
   https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers`n" -ForegroundColor Yellow
}

Write-Host "✅ Build mit:" -ForegroundColor Green
Write-Host "   Option 1 (Schnell):  powershell -ExecutionPolicy Bypass -File quick_fix.ps1"
Write-Host "   Option 2 (Detailliert): powershell -ExecutionPolicy Bypass -File fix_platformio_build.ps1 -VerboseOutput`n" -ForegroundColor Cyan

Read-Host "Drücke Enter zum Beenden"
