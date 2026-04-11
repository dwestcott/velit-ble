# Velit BLE Protocol Specification

Reverse-engineered from Bluetooth packet captures and validated against the
community Home Assistant integration (velit-hass v0.0.1) by
[John Freeborg](https://github.com/JohnFreeborg).

Both the Velit 3000R AC and diesel heater use the same GATT service but
**completely different application-layer protocols**.

## GATT Service Structure (Shared by AC and Heater)

| Component | UUID | Properties |
|-----------|------|------------|
| Service | `0000FFE0-0000-1000-8000-00805F9B34FB` | Custom Velit service |
| Write Characteristic | `0000FFE2-0000-1000-8000-00805F9B34FB` | Write, Write w/o Response |
| Notify Characteristic | `0000FFE1-0000-1000-8000-00805F9B34FB` | Notify |

BLE advertisement names: `VELIT`, `VLIT`, `D30`

---

# AC Protocol (V1.01)

## Frame Format

All commands and responses use a fixed 9-byte frame:

```
Byte:   0     1     2     3     4     5     6     7     8
      +-----+-----+-----+-----+-----+-----+-----+-----+-----+
      | 5A  | 5A  | 06  | 01  | REG | VAL | CHK | 0D  | 0A  |
      +-----+-----+-----+-----+-----+-----+-----+-----+-----+
        "Z"   "Z"   LEN   CMD   reg   val   chk   CR    LF
```

- **Checksum:** `(sum of bytes 0-5) & 0xFF`
- **Read query:** `VAL = 0x00`, device responds with current value
- **Write/set:** `VAL = desired value`, device confirms via notification
- **Min inter-command interval:** 400ms
- **Response timeout:** 3 seconds

## AC Register Map

| Reg | Name | R/W | Values | Notes |
|-----|------|-----|--------|-------|
| `0x01` | Power | R/W | `0x01`=OFF, `0x02`=ON | |
| `0x02` | Mode | R/W | See mode table | Combined mode + preset |
| `0x03` | Set Point | R/W | °C: 17-30, °F: 63-86 | Device unit preserved |
| `0x04` | Fan Speed | R/W | 1-5 | TURBO auto-sets to 5 |
| `0x07` | Ambient Temp | R | °C value (read-only) | |
| `0x0B` | Fault Info | R | Raw fault data | Format unconfirmed |
| `0x10` | Swing | R/W | `0x01`=OFF, `0x02`=ON | |

### AC Mode Values (Register 0x02)

| Value | Mode | Notes |
|-------|------|-------|
| `0x01` | Cool | Default cooling mode |
| `0x02` | Heat | May be rejected on AC-only units |
| `0x03` | Fan Only | No compressor |
| `0x04` | ECO | Energy-saving preset |
| `0x05` | Sleep | Quiet nighttime preset |
| `0x06` | Turbo | Max cooling, auto-sets fan to 5 |
| `0x07` | Dehumidify | Dry mode |
| `0x08` | Vent | Alternate ventilation |

### AC Polling

- 7 registers polled every ~6 seconds
- ~600ms between individual register queries
- Order: 0x01, 0x02, 0x03, 0x04, 0x07, 0x0B, 0x10

---

# Heater Protocol (V1.02)

## Frame Format

**Command (host → device):**
```
+----+-----+-------------------+-------------------+------+---------+-----+
| 55 | LEN | Master Addr (4B)  | Slave Addr (4B)   | FUNC | Data NB | CHK |
+----+-----+-------------------+-------------------+------+---------+-----+
```

**Response (device → host):**
```
+----+-----+-------------------+-------------------+-------+------+---------+--------+
| AA | LEN | Master Addr (4B)  | Slave Addr (4B)   | 53 46 | FUNC | Data NB | CHK 2B |
+----+-----+-------------------+-------------------+-------+------+---------+--------+
                                                      "SF"
```

- **Master Address:** `00 00 00 01` (fixed, device ignores)
- **Slave Address:** `00 00 00 2D` (fixed constant on all Velit heaters)
- **`53 46`:** Manufacturer code "SF" (present in all responses)
- **Data field:** Always **2 bytes** for single-value commands: `[value, 0x00]`. The device silently ignores commands with only 1 data byte.
- **Write checksum:** 1 byte — `sum(all preceding bytes) & 0xFF`
- **Response checksum:** 2 bytes — `sum(all preceding bytes) & 0xFFFF` (big-endian)

## Heater Function Codes

### Control Commands

| Func | Name | Data | Notes |
|------|------|------|-------|
| `0x00` | Set Mode | `0x01`=Manual, `0x02`=Thermostat | |
| `0x01` | Startup (Manual) | `0x01` | Powers on in manual mode |
| `0x02` | Shutdown | `0x00` | Powers off |
| `0x03` | Start Ventilation | `0x00` | Fan only, no combustion |
| `0x04` | Stop Ventilation | `0x00` | |
| `0x05` | Start Fuel Prime | `0x00` | 30s auto-stop |
| `0x06` | Stop Fuel Prime | `0x00` | |
| `0x07` | Set Fan Speed | `0x01`-`0x05` | Manual mode only |
| `0x08` | Set Temperature | °F or °C value | Thermostat mode |
| `0x09` | Cleaning Cycle | `0x00` | No manual stop |
| `0x0E` | Auto-Start Config | See below | Timer settings |
| `0x10` | Clock Sync | See below | Sent on connection |

### Query Commands (Polling)

| Func | Name | Data | Response Length |
|------|------|------|----------------|
| `0x0A` | Status Query | `0x00` | 7 bytes |
| `0x0B` | Sensor Query | `0x00` | 13 bytes |
| `0x0F` | Timer Query | `0x00` | Mirrors 0x0E config |

## Status Query Response (0x0A) — 7 bytes

```
[fault] [mode] [gear] [set_temp] [machine_state] [power_W] [pump_freq]
```

| Offset | Field | Notes |
|--------|-------|-------|
| 0 | Fault Code | 0=none, 1-15 (see fault table) |
| 1 | Work Mode | 1=Manual, 2=Thermostat |
| 2 | Current Gear | 1-5 |
| 3 | Set Temperature | Raw value in device unit |
| 4 | Machine State | 0-5 (see state table) |
| 5 | Heater Power | Watts |
| 6 | Pump Frequency | Divide by 10 for Hz |

## Sensor Query Response (0x0B) — 13 bytes

All 16-bit fields are big-endian. `0xFFFF` = sensor unavailable.

```
[fault] [voltage 2B] [fan_rpm 2B] [inlet 2B] [casing 2B] [outlet 2B] [altitude 2B]
```

| Offset | Field | Conversion |
|--------|-------|------------|
| 0 | Fault Code | Same as 0x0A |
| 1-2 | Supply Voltage | ÷10 for Volts (e.g., 0x0075 = 11.7V) |
| 3-4 | Fan RPM | Direct value |
| 5-6 | Inlet Temp | °F mode: raw - 60 = °F |
| 7-8 | Casing Temp | °F mode: raw - 60 = °F |
| 9-10 | Outlet Temp | °F mode: raw - 60 = °F |
| 11-12 | Altitude | ft or m depending on device unit |

## Auto-Start Config (0x0E)

```
[HOUR] [MINUTE] [??] [DURATION_min] [ENABLED] [??]
  03      00     00       B4            01      01
```

- Duration in minutes (e.g., 0xB4 = 180 = 3 hours)
- Enabled: 0x01 = on

## Clock Sync (0x10)

Sent once at BLE connection to sync the heater's clock:
```
[HH] [MM] [SS] [YY] [MM] [DD] [DOW?]
```

## Fault Codes

| Code | Fault |
|------|-------|
| 0 | No Fault |
| 1 | Ignition Failure |
| 2 | Abnormal Flame Out |
| 3 | Voltage Deviation |
| 4 | Heat Exchanger Temp Anomaly |
| 5 | Ignition Sensor Fault |
| 6 | Outlet Temp Sensor Fault |
| 7 | Fuel Pump Fault |
| 8 | Fan Fault |
| 9 | Inlet Temp Sensor Fault |
| 10 | Glow Plug Fault |
| 11 | Operating Ambient Temp Anomaly |
| 12 | Altitude Out of Range |
| 13 | Fan Blockage Fault |
| 14 | CO Pollution Exceeded |
| 15 | LIN Communication Fault |

## Machine States

| Code | State | Description |
|------|-------|-------------|
| 0 | Standby | Idle |
| 1 | Normal | Combustion active |
| 2 | Cooling Down | Post-shutdown fan cooling |
| 3 | Overtemp Standby | Paused, too hot |
| 4 | Cleaning | Fuel clearing in progress |
| 5 | Clean Complete | Cleaning finished |

## Temperature Encoding

### Set Commands
- **AC:** Direct °F or °C value, range 17-30°C / 63-86°F
- **Heater:** Direct °F or °C value, range 4-37°C / 40-99°F

### Sensor Readings (Heater only)
- **Celsius mode:** `raw - 50 = °C`
- **Fahrenheit mode:** `raw - 60 = °F`

### Unit Detection
Query current setpoint from status response:
- Value 16-30 → device in Celsius mode
- Value 61-86 → device in Fahrenheit mode
