# ESP32 WiFi-Repeater - QUICKSTART

## Installation (1 Minute)

1. **ZIP entpacken** in Projektordner
2. **Terminal öffnen** im Projektordner
3. **Build & Upload:**

```powershell
platformio run --target upload
```

## Falls Fehler: Cache-Reset

```powershell
Remove-Item "$env:USERPROFILE\.platformio\packages\framework-espidf*" -Recurse -Force -EA 0
Remove-Item "sdkconfig.esp32dev" -Force -EA 0
platformio run --target upload
```

## Hardware-Anforderungen

- **ESP32 DevKit V1** (mit USB-Kabel)
- **Router** (mit verfügbarem WLAN)

## Funktionalität

**Modus 1: Setup Portal**
- ESP32 erstellt WLAN: `ESP32-Setup`
- Browser öffnen: `192.168.4.1`
- Eingabe: Ziel-WLAN + Passwort
- Knopf halten (5s) → Speichern

**Modus 2: WiFi Repeater**
- Verbindet sich mit Ziel-WLAN
- Erstellt eigenen Hotspot mit NAT
- Vergrößert WLAN-Reichweite

## Features

✅ WiFi Repeater mit NAPT/NAT  
✅ Web-Portal zur Konfiguration  
✅ NVS-Persistierung  
✅ GPIO-Knopf zum Reset (5s halten)  
✅ DNS-Umleitung im Setup-Modus  
✅ LED-Status (optional)  

## Problemlösung

| Problem | Lösung |
|---------|--------|
| Build hängt | Terminal schließen, `quick_fix.ps1` ausführen |
| Upload fehlgeschlagen | `diagnose.ps1` ausführen, COM-Port prüfen |
| Kein WLAN zu sehen | USB-Kabel prüfen, Treiber installieren (CH340) |
| Framework-Fehler | `platformio run -v` für Details |

## Dateien

```
esp32-wifi-repeater/
├── src/
│   └── main.c           (Firmware-Code)
├── platformio.ini       (PlatformIO-Config)
├── sdkconfig.defaults   (ESP-IDF-Config)
├── CMakeLists.txt       (Build-Config)
├── quick_fix.ps1        (Schnell-Fix Skript)
├── fix_platformio_build.ps1
├── diagnose.ps1         (System-Diagnose)
└── README_SKRIPTE.md    (Skript-Dokumentation)
```

## Support

Verbose Build für Fehleranalyse:
```powershell
platformio run -v
```

Detaillierter Upload-Log:
```powershell
platformio run --target upload -v 2>&1 | Tee-Object build.log
```
