# Velit BLE Protocol Specification

Reverse-engineered from Bluetooth packet captures of the Velit iOS/macOS app communicating with a Velit 3000R air conditioner.

## GATT Service Structure

| Component | UUID | Handle | Properties |
|-----------|------|--------|------------|
| Service | `0xFFE0` | 0x000e–0x0013 | Custom Velit service |
| Write Characteristic | `0xFFE2` | 0x0010 | Write, Write w/o Response |
| Notify Characteristic | `0xFFE1` | 0x0012 | Notify |
| CCCD Descriptor | `0x2902` | 0x0013 | Enable notifications |

- **Write to `0xFFE2`** to send commands to the device
- **Subscribe to `0xFFE1`** notifications to receive responses

## Frame Format

All commands and responses use a 9-byte frame:

```
Byte:   0     1     2     3     4     5     6     7     8
      ┌─────┬─────┬─────┬─────┬─────┬─────┬─────┬─────┬─────┐
      │ 5A  │ 5A  │ 06  │ 01  │ REG │ VAL │ CHK │ 0D  │ 0A  │
      └─────┴─────┴─────┴─────┴─────┴─────┴─────┴─────┴─────┘
        "Z"   "Z"   LEN   CMD   reg   val   chk   CR    LF
```

| Field | Bytes | Description |
|-------|-------|-------------|
| Header | `5A 5A` | ASCII "ZZ" — start of frame marker |
| Length | `06` | Number of bytes following (always 6 for single-register frames) |
| Command | `01` | Command type (always 0x01 in observed traffic) |
| Register | 1 byte | Register address (see table below) |
| Value | 1 byte | `0x00` = read query; non-zero = write/set value |
| Checksum | 1 byte | `(sum of bytes 0–5) & 0xFF` |
| Terminator | `0D 0A` | CR+LF |

### Checksum Calculation

```python
checksum = (0x5A + 0x5A + 0x06 + 0x01 + register + value) & 0xFF
```

### Read vs Write

- **Read (query):** Send frame with `VAL = 0x00`. Device responds via notification with current value.
- **Write (set):** Send frame with `VAL = desired value`. Device confirms via notification.

## Register Map — Velit 3000R AC

| Register | Name | R/W | Values | Notes |
|----------|------|-----|--------|-------|
| `0x01` | Power | R/W | `0x01` = OFF, `0x02` = ON | Uses 1/2 since 0x00 is reserved for read queries |
| `0x02` | Mode / Preset | R/W | See mode table below | Combined mode + preset in single register |
| `0x03` | Set Point Temp | R/W | 61–86 | Stored in °F |
| `0x04` | Fan Speed | R/W | 1–5 | TURBO auto-sets to 5 |
| `0x07` | Ambient Temp | R | 0–50+ | Stored in °C (read-only sensor) |
| `0x0B` | Status | R | `0x00` = normal | May indicate error conditions |
| `0x10` | Swing | R/W | `0x01` = OFF, `0x02` = ON | Swing vane position |

### Mode Register (0x02) Values

| Value | Mode | Notes |
|-------|------|-------|
| `0x01` | Cooling (AUTO) | Default cooling mode |
| `0x02` | Heating | May be rejected on AC-only units (auto-switches to Fan) |
| `0x03` | Fan Only | Fan runs without compressor |
| `0x04` | ECO | Energy-saving cooling mode |
| `0x05` | SLEEP | Quiet nighttime mode |
| `0x06` | TURBO | Maximum cooling — auto-sets fan speed to 5 |
| `0x08` | Vent Toggle | Momentary command — does not persist as a mode |

## Polling Behavior

The stock Velit app polls all 7 registers in a repeating cycle:

- **Cycle interval:** ~6 seconds between full cycles
- **Inter-register delay:** ~600ms between individual register queries
- **Query order:** 0x01, 0x02, 0x03, 0x04, 0x07, 0x10, 0x0B

## Example Commands

### Turn ON
```
TX: 5A 5A 06 01 01 02 BE 0D 0A
RX: 5A 5A 06 01 01 02 BE 0D 0A  (confirmed)
```

### Set temperature to 72°F
```
TX: 5A 5A 06 01 03 48 AC 0D 0A
RX: 5A 5A 06 01 03 48 AC 0D 0A  (confirmed)
```

### Set fan speed to 3
```
TX: 5A 5A 06 01 04 03 C2 0D 0A
RX: 5A 5A 06 01 04 03 C2 0D 0A  (confirmed)
```

### Query ambient temperature
```
TX: 5A 5A 06 01 07 00 C2 0D 0A
RX: 5A 5A 06 01 07 0F D1 0D 0A  (15°C = 59°F)
```
