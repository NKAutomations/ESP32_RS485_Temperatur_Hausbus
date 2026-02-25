# ESP32_RS485_Temperatur_Hausbus

# esp32_Temp_RS485

ESP32-Projekt zum Auslesen von **DS18B20** Temperatursensoren und Senden der Werte als Telegramme über **RS485** (UART/Serial2).  
Zusätzlich gibt es einen **Web-Konfigurator** (Access Point), um Bus-Parameter und Sensor-Instanzen zu konfigurieren.

## Features

- **DS18B20** (OneWire) Temperatursensoren auslesen
- **RS485** Ausgabe über `Serial2` (ESP32 UART2)
- Telegramm-Rahmen: `0xFD <PAYLOAD> 0xFE`
- Payload-Format (Temperatur):
  - `<DEVICE>.TMP.<INST>.STATUS.<P1>.<P2>`
  - Beispiel: `5511.TMP.1.STATUS.26.94`
- **Web UI im AP-Modus** (ESP32 erstellt eigenes WLAN)
- Persistente Speicherung via **Preferences (NVS)**:
  - `deviceId`
  - `txIntervalMs`
  - Mapping: Sensor-ROM → Instance (1..255)

## Hardware

- ESP32 (beliebiges DevKit)
- 1..n DS18B20 Sensoren
- RS485 TTL Adapter (TX/RX) an UART2

### Pinout (Default)

| Funktion | ESP32 GPIO |
|---------|------------|
| DS18B20 Data | GPIO 5 |
| RS485 RX (Serial2) | GPIO 22 |
| RS485 TX (Serial2) | GPIO 23 |

> Hinweis: RX/TX am RS485-TTL Adapter sind typischerweise **gekreuzt** anzuschließen (ESP32 TX → Adapter RX, ESP32 RX → Adapter TX).

## RS485 / Bus

- UART: `Serial2`
- Baudrate: `56700`
- Frame: Start `0xFD`, Ende `0xFE`

## Web-Konfigurator

Der ESP32 startet als Access Point:

- SSID: `ESP32-TMP-SETUP`
- Passwort: offen (leer)

Im Browser öffnen:

- `http://192.168.4.1/`

Dort kannst du:
- `Device ID` setzen
- Sendeintervall (`txIntervalMs`) setzen
- Sensoren sehen und **ROM → Instance** zuweisen

## API Endpoints (Kurzüberblick)

- `GET /`  
  Web UI

- `GET /api/buscfg`  
  Liefert Bus-Konfiguration (Device ID, Intervall)

- `POST /api/buscfg`  
  Speichert Bus-Konfiguration

- `GET /api/sensors`  
  Listet gefundene DS18B20 Sensoren (ROM, Temperatur, Instance)

- `POST /api/map`  
  Speichert Mapping `rom` → `instance`

## Build / Flash

1. Arduino IDE oder PlatformIO verwenden
2. Libraries installieren:
   - `OneWire`
   - `DallasTemperature`
3. Board: passendes ESP32-Board auswählen
4. Sketch flashen

## Betrieb

- ESP32 startet AP + Webserver
- DS18B20 werden zyklisch gelesen
- Telegramme werden im eingestellten Intervall über RS485 gesendet

## Troubleshooting

- **Keine Sensoren gefunden**
  - Pullup-Widerstand am DS18B20 Data (typ. 4.7k) prüfen
  - GPIO 5 Verkabelung prüfen

- **Keine RS485 Telegramme**
  - RX/TX gekreuzt?
  - Baudrate stimmt (56700)?
  - GND zwischen ESP32 und RS485-Adapter verbunden?

## Lizenz

Wähle eine Lizenz (z.B. MIT) und ergänze sie als `LICENSE` Datei.
