ESP32 Captive Config Portal + Wi-Fi Repeater

Funktionen:
- Startet nach dem Boot im Config-AP-Modus, wenn keine gültige Konfiguration vorhanden ist.
- Captive portal per DNS-Redirect.
- Speichert Target-WLAN und Hotspot-Daten in NVS.
- Startet nach dem Speichern neu.
- Im Router-Modus verbindet sich der ESP32 als Station mit dem Target-WLAN und stellt gleichzeitig ein Hotspot-WLAN bereit.
- IPv4 NAT ist aktiviert.
- BOOT-Taste 5 Sekunden halten: Konfiguration löschen und neu starten.

Wichtige Annahmen:
- BOOT-Taste ist GPIO0.
- Board: ESP32 Dev Module.

Build:
- PlatformIO öffnen.
- Projektordner importieren.
- Flashen.
