# Quick-Fix: Schnelle Behebung des PlatformIO Build-Fehlers
# Rechtsklick → "Mit PowerShell ausführen"

Write-Host "`n🔧 ESP32 PlatformIO Quick-Fix`n" -ForegroundColor Cyan

# Admin-Check
if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host "❌ Admin-Rechte erforderlich!`n" -ForegroundColor Red
    exit 1
}

$userProfile = $env:USERPROFILE

# 1. Cache löschen
Write-Host "1️⃣  Lösche PlatformIO Cache..." -ForegroundColor Yellow
Remove-Item "$userProfile\.platformio\packages" -Recurse -Force -EA 0
Remove-Item "$userProfile\.platformio\cache" -Recurse -Force -EA 0
Write-Host "   ✅ Cache gelöscht`n" -ForegroundColor Green

# 2. Core Update
Write-Host "2️⃣  Aktualisiere PlatformIO Core..." -ForegroundColor Yellow
& cmd /c "platformio core update" 2>&1 | Out-Null
Write-Host "   ✅ Core aktualisiert`n" -ForegroundColor Green

# 3. Clean
Write-Host "3️⃣  Bereinige Projekt..." -ForegroundColor Yellow
& cmd /c "platformio run -t clean" 2>&1 | Out-Null
Write-Host "   ✅ Projekt bereinigt`n" -ForegroundColor Green

# 4. Build
Write-Host "4️⃣  Starte Build..." -ForegroundColor Yellow
& cmd /c "platformio run" 2>&1
$buildSuccess = $LASTEXITCODE -eq 0

if ($buildSuccess) {
    Write-Host "`n✅ Build erfolgreich!`n" -ForegroundColor Green
    
    # 5. Upload
    Write-Host "5️⃣  Starte Upload auf ESP32..." -ForegroundColor Yellow
    & cmd /c "platformio run --target upload" 2>&1
    
    if ($LASTEXITCODE -eq 0) {
        Write-Host "`n✅ Upload erfolgreich! WiFi-Repeater lädt...`n" -ForegroundColor Green
    } else {
        Write-Host "`n⚠️  Upload fehlgeschlagen`n" -ForegroundColor Red
        Write-Host "Fehlerursachen:
- USB-Kabel nicht verbunden
- ESP32 in nicht erkannt (Treiber?)
- Bereits anderes Programm auf COM-Port zugegriffen" -ForegroundColor Yellow
    }
} else {
    Write-Host "`n❌ Build fehlgeschlagen`n" -ForegroundColor Red
    Write-Host "Für ausführlichen Output folgende Zeile ausführen:" -ForegroundColor Yellow
    Write-Host "  platformio run -v`n" -ForegroundColor Cyan
}

Read-Host "Drücke Enter zum Beenden"
