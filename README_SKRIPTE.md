# ESP32 WiFi-Repeater - PlatformIO Build-Fehler Behebung

## Fehler
```
MissingPackageManifestError: Could not find one of 'package.json' manifest files in the package
```

**Ursache:** Beschädigte/unvollständige ESP-IDF Framework-Installation in PlatformIO Cache

---

## 3 PowerShell-Skripte zur Auswahl

### 1️⃣ **quick_fix.ps1** (EMPFOHLEN - 2 Minuten)
Schnellste Lösung für sofortige Behebung.

**Ausführung:**
```powershell
powershell -ExecutionPolicy Bypass -File quick_fix.ps1
```

Oder: `quick_fix.ps1` Rechtsklick → "Mit PowerShell ausführen"

**Was es tut:**
- Löscht PlatformIO Cache
- Aktualisiert PlatformIO Core
- Bereinigt Projekt
- Startet Build
- Startet Upload (automatisch, falls Build erfolgreich)

---

### 2️⃣ **diagnose.ps1** (Für Problemanalyse)
Überprüft Voraussetzungen und identifiziert Probleme.

**Ausführung:**
```powershell
powershell -ExecutionPolicy Bypass -File diagnose.ps1
```

**Prüft:**
- ✅ PlatformIO Installation
- ✅ Python Abhängigkeit
- ✅ Projektstruktur (alle Dateien vorhanden)
- ✅ ESP-IDF Framework Status
- ✅ USB/COM-Ports (ESP32 Verbindung)
- ✅ Speicher & Ressourcen

---

### 3️⃣ **fix_platformio_build.ps1** (Erweitert - mit Optionen)
Detaillierte Kontrolle mit optionalen Parametern.

**Ausführung (Standard):**
```powershell
powershell -ExecutionPolicy Bypass -File fix_platformio_build.ps1
```

**Mit ausführlichem Output (Fehlerdiagnose):**
```powershell
powershell -ExecutionPolicy Bypass -File fix_platformio_build.ps1 -VerboseOutput
```

**Cache-Löschung überspringen:**
```powershell
powershell -ExecutionPolicy Bypass -File fix_platformio_build.ps1 -SkipCleanCache
```

**Kombiniert:**
```powershell
powershell -ExecutionPolicy Bypass -File fix_platformio_build.ps1 -VerboseOutput
```

---

## Schritt-für-Schritt Handlungsanleitung

### Problem 1: Erste Build-Fehlgeschlag
1. Führe aus: `quick_fix.ps1`
2. Warte auf Abschluss
3. Falls noch Fehler → weiter zu Problem 2

### Problem 2: Fehler nach Quick-Fix
1. Führe aus: `diagnose.ps1`
2. Lies die Empfehlungen
3. Führe betreffendes Fix-Skript erneut aus

### Problem 3: USB-Verbindungsfehler
```
Upload fehlgeschlagen
```

**Ursachen & Lösungen:**
| Problem | Lösung |
|---------|--------|
| ESP32 nicht erkannt | Treiber installieren: https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers |
| Falscher COM-Port | Prüfe mit `diagnose.ps1`, trage Port in `platformio.ini` ein: `upload_port = COM3` |
| Anderes Programm nutzt Port | Serial-Monitor/IDE schließen, IDE neu starten |
| USB-Kabel defekt | Anderes Kabel testen |

---

## Administratorrechte

**Alle Skripte benötigen Admin-Rechte!**

Optionen:
1. PowerShell als Admin öffnen → Skript ausführen
2. Skript mit Parameter aufrufen (oben gezeigt)
3. Rechtklick auf .ps1 → "Mit PowerShell ausführen"

Falls "Zugriff verweigert"-Fehler:
```powershell
Set-ExecutionPolicy -ExecutionPolicy RemoteSigned -Scope CurrentUser
```

---

## Backup & Alternative

Sollten alle Skripte fehlschlagen:

### Manuelle Methode
```powershell
# 1. Cache löschen
Remove-Item "$env:USERPROFILE\.platformio\packages" -Recurse -Force
Remove-Item "$env:USERPROFILE\.platformio\cache" -Recurse -Force

# 2. Core update
platformio core update

# 3. Projekt bereinigen
platformio run -t clean

# 4. Build mit Verbose
platformio run -v

# 5. Upload
platformio run --target upload
```

### Projekt-Reset
Falls Projektstruktur kaputt:
```powershell
# Neues Projekt initialisieren
platformio init --board esp32dev --framework espidf

# main.c-Code aus src/main.c kopieren in:
# .pio\build\esp32dev\...
```

---

## Output-Interpretation

### ✅ Erfolgreiche Ausführung
```
[HH:MM:SS] Build erfolgreich
[HH:MM:SS] Upload erfolgreich! WiFi-Repeater lädt...
```
→ Fertig! ESP32 startet Repeater-Firmware

### ⚠️ Cache-Löschung fehlgeschlagen
```
[HH:MM:SS] Fehler beim Cache-Löschen
  → PlatformIO IDE/VS Code schließen und erneut versuchen
```
→ Schließe alle IDEs, führe Skript erneut aus

### ❌ Build-Fehler
```
[HH:MM:SS] Build fehlgeschlagen (Code: 1)
```
→ Führe aus: `fix_platformio_build.ps1 -VerboseOutput`
→ Lese Output-Details zur Fehleridentifikation

### ❌ Upload-Fehler
```
Upload fehlgeschlagen
Fehlerursachen:
- USB-Kabel nicht verbunden
- ESP32 nicht erkannt
```
→ Führe aus: `diagnose.ps1`
→ Prüfe COM-Port Status

---

## Sicherheit

- ✅ Alle Skripte sind lokal (kein Internet-Zugriff)
- ✅ Keine externen Abhängigkeiten außer PlatformIO
- ✅ Nur Dateien löschen, die PlatformIO selbst erzeugt hat
- ✅ Projektquellcode (`src/`) wird nicht verändert

---

## Support

**Verbose-Output für Ticketing:**
```powershell
# Speichert Output in Datei
powershell -ExecutionPolicy Bypass -File fix_platformio_build.ps1 -VerboseOutput *>&1 | Tee-Object -FilePath build_log.txt
```

Datei `build_log.txt` enthält vollständigen Build-Log zur Fehleranalyse.
