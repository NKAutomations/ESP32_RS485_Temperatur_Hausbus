```md
# ESP32Temp_HausBusV2 — README (Version 2.1.0)

ESP32 + DS18B20 (1‑Wire) + RS485 HausBus + Web-UI (Access Point).

Dieses Projekt kann:

- Temperaturen **zyklisch** (Auto‑Send) über RS485 senden (**nicht-blockierend**, Scheduler/State-Machine)
- Temperaturen **auf Anfrage** (On‑Demand) über RS485 beantworten
- **SYS‑Kommandos** über RS485 verarbeiten (z. B. WiFi/Web an/aus)
- eine **Web‑GUI** im **Access Point (AP)** bereitstellen (Konfiguration + Sensor‑Mapping)
- **Ping/Alive** Telegramme (Template-basiert) beantworten

---

## Inhalt

- [1. RS485 Telegramm-Format (Framing)](#1-rs485-telegramm-format-framing)
- [2. RS485 Befehle — Senden (TX)](#2-rs485-befehle--senden-tx)
- [3. RS485 Befehle — Empfangen (RX) & Reaktion](#3-rs485-befehle--empfangen-rx--reaktion)
- [4. Web-GUI (AP)](#4-web-gui-ap)
- [5. Sensor-Instanzen / Mapping (ROM → Instance)](#5-sensor-instanzen--mapping-rom--instance)
- [6. Persistente Einstellungen (Preferences)](#6-persistente-einstellungen-preferences)
- [7. Busverhalten / Arbitration](#7-busverhalten--arbitration)
- [8. Troubleshooting](#8-troubleshooting)
- [9. Command Cheat Sheet](#9-command-cheat-sheet)

---

## 1. RS485 Telegramm-Format (Framing)

Alle RS485-Nachrichten werden **gerahmt**:

- **Start-Byte:** `0xFD`
- **Payload:** ASCII-Text (z. B. `1.TMP.3.STATUS.21.50`)
- **End-Byte:** `0xFE`

**Beispiel (Payload):**
```
1.TMP.1.STATUS.21.34
```

**Auf dem Bus (Bytefolge):**
```
FD 31 2E 54 4D 50 ... FE
```

> Wichtig: Der Parser verarbeitet nur Payloads, die korrekt zwischen `0xFD` und `0xFE` ankommen.

---

## 2. RS485 Befehle — Senden (TX)

### 2.1 Temperatur-Status (Auto‑Send / zyklisch)

Wenn Auto‑Send aktiv ist, sendet der ESP32 für jeden Sensor:

**Format**
```
<DeviceID>.TMP.<Inst>.STATUS.<P1>.<P2>
```

- `<DeviceID>`: Geräte-ID des ESP32 (1..9999)
- `<Inst>`: Sensor-Instanz (1..255), aus Mapping (ROM → Instanz)
- `<P1>`: Ganzzahlanteil der Temperatur (°C)
- `<P2>`: Nachkommanteil (0..99), immer positiv

**Beispiel**
```
1.TMP.2.STATUS.23.75
```
→ Instanz 2 hat **23.75 °C**

**Ungültig/Fehler**
Wenn Temperatur außerhalb plausibler Grenzen ist:
- `P1 = -127`, `P2 = 0`

---

### 2.2 Temperatur-Status (Antwort auf Anfrage)

Bei einem On‑Demand Request (siehe RX) antwortet der ESP32 ebenfalls mit:

```
<DeviceID>.TMP.<Inst>.STATUS.<P1>.<P2>
```

---

### 2.3 SYS rCONFIG (Antwort auf Konfig-Abfrage)

Bei einer SYS-Abfrage sendet der ESP32:

**Format**
```
<DeviceID>.SYS.1.rCONFIG.<P1>.<VALUE>
```

Aktuell implementiert:

- `P1 = 1` → WiFi/Web **Runtime-Status**
- `VALUE`:
  - `1` = AP + Webserver laufen
  - `0` = aus

**Beispiel**
```
1.SYS.1.rCONFIG.1.1
```

---

### 2.4 Ping Reply (Template-basiert)

Wenn ein Ping-Request empfangen wird, sendet der ESP32 einen Ping-Reply.

Default Templates:

- RX erwartet: `DeviceID.OUT.210.gSTATUS`
- TX sendet:   `DeviceID.OUT.210.rSTATUS`

`DeviceID` wird automatisch durch die konfigurierte Geräte-ID ersetzt.

**Beispiel bei DeviceID=1**
- RX: `1.OUT.210.gSTATUS`
- TX: `1.OUT.210.rSTATUS`

---

## 3. RS485 Befehle — Empfangen (RX) & Reaktion

### 3.1 Temperatur anfordern (On‑Demand)

**RX**
```
<DeviceID>.TMP.<Inst>.gSTATUS
```

**Reaktion**
- ESP32 liest Temperatur des Sensors mit Instanz `<Inst>`
- TX Antwort:
  ```
  <DeviceID>.TMP.<Inst>.STATUS.<P1>.<P2>
  ```

**Beispiel**
- RX:
  ```
  1.TMP.2.gSTATUS
  ```
- TX:
  ```
  1.TMP.2.STATUS.23.75
  ```

Wenn `<Inst>` nicht gemappt/gefunden wird: **keine Antwort** (wird im Serial Log angezeigt).

---

### 3.2 SYS: WiFi/Web an/aus (SET_CFG)

**RX**
```
<DeviceID>.SYS.1.SET_CFG.<P1>.<P2>
```

Aktuell implementiert:

- `P1 = 1` → WiFi/Web Enable
- `P2 = 0` → WiFi/Web AUS
- `P2 = 1` → WiFi/Web AN

**Beispiele**
- AUS:
  ```
  1.SYS.1.SET_CFG.1.0
  ```
- AN:
  ```
  1.SYS.1.SET_CFG.1.1
  ```

**Hinweis:** Das Umschalten erfolgt “bombproof” im `loop()` (nicht direkt im RX-Handler), um die WiFi-Stack Stabilität zu erhöhen.

---

### 3.3 SYS: Konfiguration abfragen (gCONFIG → rCONFIG)

**RX**
```
<DeviceID>.SYS.1.gCONFIG.<P1>
```

**TX**
```
<DeviceID>.SYS.1.rCONFIG.<P1>.<VALUE>
```

Aktuell implementiert:

- `P1 = 1` → WiFi/Web Runtime-Status
- `VALUE = 1` wenn AP+Webserver laufen, sonst `0`

**Beispiel**
- RX:
  ```
  1.SYS.1.gCONFIG.1
  ```
- TX:
  ```
  1.SYS.1.rCONFIG.1.1
  ```

---

### 3.4 Ping Request (Template)

**RX (Default)**
```
DeviceID.OUT.210.gSTATUS
```

**TX (Default)**
```
DeviceID.OUT.210.rSTATUS
```

Die Templates sind in der GUI konfigurierbar. `DeviceID` wird ersetzt.

---

## 4. Web-GUI (AP)

Wenn WiFi/Web aktiviert ist, startet der ESP32 einen Access Point:

- **SSID:** `ESP32-TMP-SETUP`
- **Passwort:** leer (Open AP; im Code setzbar)
- **IP:** typischerweise `192.168.4.1`

### 4.1 Bus Settings

In der GUI konfigurierbar (und persistent gespeichert):

- **Device ID** (1..9999)
- **Send interval (ms)** (`0` = aus)
- **Auto send enabled**
- **Gap between sensor telegrams (ms)**
- **Ping response template** (TX)
- **Ping request template** (RX)

### 4.2 Sensors / Mapping

Die GUI zeigt alle gefundenen DS18B20:

- ROM (16 Hex Zeichen)
- Temperatur (°C)
- Instance (1..255) editierbar

Damit legst du fest, welche Instanznummer im Bus-Protokoll verwendet wird.

---

## 5. Sensor-Instanzen / Mapping (ROM → Instance)

Jeder DS18B20 hat eine eindeutige ROM-ID.  
Das Projekt speichert ein Mapping **ROM → Instance**, damit die Instanz stabil bleibt, auch wenn sich die Reihenfolge am 1‑Wire Bus ändert.

---

## 6. Persistente Einstellungen (Preferences)

Gespeichert im ESP32 NVS (Preferences, Namespace `tmpcfg`):

- `dev_id` (DeviceID)
- `tx_ms` (Intervall)
- `auto_en` (Auto-Send)
- `gap_ms` (Gap)
- `wifi_en` (WiFi/Web desired)
- `ping_tx`, `ping_rx` (Ping Templates)
- ROM→Instance Mapping (intern über Hash-Key)

---

## 7. Busverhalten / Arbitration

Um Kollisionen zu reduzieren:

- Bus-Idle-Erkennung über Zeit seit letztem RX-Byte
- Random Backoff vor dem Senden
- Holdoff nach Antworten (SYS/Ping/On-Demand), damit Auto-Send nicht sofort dazwischenfunkt

---

## 8. Troubleshooting

### Webserver nicht erreichbar
- Per RS485 prüfen:
  - RX: `<DeviceID>.SYS.1.gCONFIG.1`
  - TX: `<DeviceID>.SYS.1.rCONFIG.1.<0|1>`
- Serial Logs prüfen:
  - `[WIFI] AP start OK | IP: 192.168.4.1`
  - `[WEB] started`

### Keine Antwort auf `.gSTATUS`
- Instanz existiert? (GUI → Sensorliste)
- Sensoren gefunden? (`[DS18B20] found devices: ...`)
- Framing korrekt? (`0xFD ... 0xFE`)

---

## 9. Command Cheat Sheet

### Temperatur (On-Demand)
- **Request (RX):**
  ```
  <DeviceID>.TMP.<Inst>.gSTATUS
  ```
- **Reply (TX):**
  ```
  <DeviceID>.TMP.<Inst>.STATUS.<P1>.<P2>
  ```

### SYS WiFi/Web
- **Set OFF (RX):**
  ```
  <DeviceID>.SYS.1.SET_CFG.1.0
  ```
- **Set ON (RX):**
  ```
  <DeviceID>.SYS.1.SET_CFG.1.1
  ```
- **Get (RX):**
  ```
  <DeviceID>.SYS.1.gCONFIG.1
  ```
- **Reply (TX):**
  ```
  <DeviceID>.SYS.1.rCONFIG.1.<0|1>
  ```

### Ping (Default Templates)
- **Request (RX):**
  ```
  <DeviceID>.OUT.210.gSTATUS
  ```
- **Reply (TX):**
  ```
  <DeviceID>.OUT.210.rSTATUS
  ```
```
