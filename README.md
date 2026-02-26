# ESP32Temp_HausBusV2 — README (Version 2.1.0)

ESP32 + DS18B20 (1-Wire) + RS485 HausBus + Web-UI (Access Point).

## Features

- **Zyklisches Senden** von Temperaturen (Auto-Send) über RS485
  → **nicht-blockierend** (Scheduler/State-Machine)
- **On-Demand Temperaturabfrage** per RS485 (`.gSTATUS` → `.STATUS`)
- **SYS-Kommandos** per RS485 (z. B. WiFi/Web an/aus)
- **Web-GUI** im **Access Point (AP)** zur Konfiguration & Sensor-Mapping
- **Ping/Alive** (Template-basiert) Request/Reply

---

## Inhalt

- [RS485 Telegramm-Format (Framing)](#rs485-telegramm-format-framing)
- [RS485 Befehle — Senden (TX)](#rs485-befehle--senden-tx)
- [RS485 Befehle — Empfangen (RX) & Reaktion](#rs485-befehle--empfangen-rx--reaktion)
- [Web-GUI (AP)](#web-gui-ap)
- [Sensor-Instanzen / Mapping (ROM → Instance)](#sensor-instanzen--mapping-rom--instance)
- [Persistente Einstellungen (Preferences)](#persistente-einstellungen-preferences)
- [Busverhalten / Arbitration](#busverhalten--arbitration)
- [Troubleshooting](#troubleshooting)
- [Command Cheat Sheet](#command-cheat-sheet)

---

## RS485 Telegramm-Format (Framing)

Alle RS485 Payloads sind ASCII und werden gerahmt:

- Start: `0xFD`
- Payload: z. B. `1.TMP.2.STATUS.23.75`
- End: `0xFE`

**Beispiel (Payload):**
`1.TMP.2.STATUS.23.75`

**Beispiel (Bytes auf dem Bus):**
`FD 31 2E 54 4D 50 2E 32 2E 53 54 41 54 55 53 2E 32 33 2E 37 35 FE`

---

## RS485 Befehle — Senden (TX)

### Temperatur Status (Auto-Send / zyklisch)

Wird gesendet, wenn Auto-Send aktiv ist.

**Format**
`<DeviceID>.TMP.<Inst>.STATUS.<P1>.<P2>`

- `<DeviceID>`: 1..9999 (konfigurierbar)
- `<Inst>`: 1..255 (Sensor-Instanz aus ROM→Instance Mapping)
- Temperatur wird als **P1.P2** gesendet:
  - `P1` = Ganzzahlanteil (°C)
  - `P2` = Nachkommanteil (0..99), immer positiv

**Beispiel**
`1.TMP.2.STATUS.23.75`

**Fehlerwert**
Wenn Temperatur außerhalb plausibler Grenzen ist:
`<DeviceID>.TMP.<Inst>.STATUS.-127.0`

---

### Temperatur Status (Antwort auf On-Demand)

Antwort auf einen `.gSTATUS` Request (siehe RX).

**Format**
`<DeviceID>.TMP.<Inst>.STATUS.<P1>.<P2>`

---

### SYS Konfig Antwort (rCONFIG)

Antwort auf `.gCONFIG` (siehe RX).

**Format**
`<DeviceID>.SYS.1.rCONFIG.<P1>.<VALUE>`

Aktuell implementiert:

- `P1 = 1` → WiFi/Web **Runtime-Status**
- `VALUE`:
  - `1` = AP + Webserver laufen
  - `0` = aus

**Beispiel**
`1.SYS.1.rCONFIG.1.1`

---

### Ping Reply (Template-basiert)

Wenn ein Ping-Request empfangen wird, sendet der ESP32 einen Reply.

Default Templates:

- RX erwartet: `DeviceID.OUT.210.gSTATUS`
- TX sendet:   `DeviceID.OUT.210.rSTATUS`

`DeviceID` wird automatisch durch die konfigurierte Geräte-ID ersetzt.

**Beispiel bei DeviceID=1**
- RX: `1.OUT.210.gSTATUS`
- TX: `1.OUT.210.rSTATUS`

---

## RS485 Befehle — Empfangen (RX) & Reaktion

### Temperatur anfordern (On-Demand)

**RX**
`<DeviceID>.TMP.<Inst>.gSTATUS`

**Reaktion (TX)**
`<DeviceID>.TMP.<Inst>.STATUS.<P1>.<P2>`

**Beispiel**
- RX:
`1.TMP.2.gSTATUS`
- TX:
`1.TMP.2.STATUS.23.75`

**Hinweis:** Wenn `<Inst>` nicht gemappt/gefunden wird: **keine Antwort**.

---

### SYS: WiFi/Web an/aus (SET_CFG)

**RX**
`<DeviceID>.SYS.1.SET_CFG.<P1>.<P2>`

Implementiert:

- `P1 = 1` → WiFi/Web Enable
- `P2 = 0` → OFF
- `P2 = 1` → ON

**Beispiele**
- OFF:
`1.SYS.1.SET_CFG.1.0`
- ON:
`1.SYS.1.SET_CFG.1.1`

**Wichtig**
- Dieses SET sendet **keine direkte Bestätigung**.
- Umschalten erfolgt stabil im `loop()` (nicht im RX-Handler).

---

### SYS: Konfiguration abfragen (gCONFIG)

**RX**
`<DeviceID>.SYS.1.gCONFIG.<P1>`

Implementiert:

- `P1 = 1` → WiFi/Web Runtime-Status

**Reaktion (TX)**
`<DeviceID>.SYS.1.rCONFIG.1.<0|1>`

**Beispiel**
- RX:
`1.SYS.1.gCONFIG.1`
- TX:
`1.SYS.1.rCONFIG.1.0`

---

### Ping Request (Template)

**RX (Default)**
`DeviceID.OUT.210.gSTATUS`

**Reaktion (TX Default)**
`DeviceID.OUT.210.rSTATUS`

---

## Web-GUI (AP)

Wenn WiFi/Web aktiviert ist, startet der ESP32 einen Access Point:

- **SSID:** `ESP32-TMP-SETUP`
- **Passwort:** leer (Open AP; im Code setzbar)
- **URL:** `http://192.168.4.1/` (typisch)

### Bus Settings

Konfigurierbar (und persistent gespeichert):

- Device ID (1..9999)
- Send interval (ms) (`0` = aus)
- Auto send enabled
- Gap between sensor telegrams (ms)
- Ping request template (RX)
- Ping response template (TX)

### Sensors / Mapping

Die GUI zeigt alle gefundenen DS18B20:

- ROM (16 Hex Zeichen)
- Temperatur
- Instance (1..255) editierbar

Damit legst du fest, welche Instanznummer im Bus-Protokoll verwendet wird.

---

## Sensor-Instanzen / Mapping (ROM → Instance)

Jeder DS18B20 hat eine eindeutige ROM-ID.
Das Projekt speichert ein Mapping **ROM → Instance**, damit die Instanz stabil bleibt, auch wenn sich die Reihenfolge am 1-Wire Bus ändert.

---

## Persistente Einstellungen (Preferences)

Gespeichert im ESP32 NVS (Namespace `tmpcfg`):

- `dev_id` → DeviceID
- `tx_ms` → Intervall
- `auto_en` → Auto-Send
- `gap_ms` → Gap
- `wifi_en` → WiFi/Web desired
- `ping_tx`, `ping_rx` → Ping Templates
- ROM→Instance Mapping (intern über Hash-Key)

---

## Busverhalten / Arbitration

Um Kollisionen zu reduzieren:

- Bus-Idle Erkennung (Zeit seit letztem RX-Byte)
- Random Backoff vor dem Senden
- Holdoff nach Antworten (SYS/Ping/On-Demand), damit Auto-Send nicht sofort dazwischenfunkt

---

## Troubleshooting

### Webserver/AP nicht erreichbar

1. Per RS485 prüfen:
   - RX:
   `<DeviceID>.SYS.1.gCONFIG.1`
   - TX:
   `<DeviceID>.SYS.1.rCONFIG.1.<0|1>`
2. Serial Logs prüfen:
   - `[WIFI] AP start OK | IP: 192.168.4.1`
   - `[WEB] started`

### Keine Antwort auf `.gSTATUS`

- Instanz existiert? (GUI → Sensorliste / Mapping)
- Sensoren gefunden? (`[DS18B20] found devices: ...`)
- Framing korrekt? (`0xFD ... 0xFE`)

---

## Command Cheat Sheet

### Temperatur (On-Demand)
**Request (RX)**
`<DeviceID>.TMP.<Inst>.gSTATUS`

**Reply (TX)**
`<DeviceID>.TMP.<Inst>.STATUS.<P1>.<P2>`

### SYS WiFi/Web
**Set OFF (RX)**
`<DeviceID>.SYS.1.SET_CFG.1.0`

**Set ON (RX)**
`<DeviceID>.SYS.1.SET_CFG.1.1`

**Get (RX)**
`<DeviceID>.SYS.1.gCONFIG.1`

**Reply (TX)**
`<DeviceID>.SYS.1.rCONFIG.1.<0|1>`

### Ping (Default Templates)
**Request (RX)**
`<DeviceID>.OUT.210.gSTATUS`

**Reply (TX)**
`<DeviceID>.OUT.210.rSTATUS`
