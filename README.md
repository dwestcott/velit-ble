# Velit Bluetooth — ESPHome BLE Controller

Custom ESPHome components for controlling [Velit](https://velitcamping.com) camping HVAC products via Bluetooth Low Energy, enabling remote control through Home Assistant over WiFi/VPN.

Protocols were reverse-engineered from Bluetooth packet captures and validated against the community Home Assistant integration by [John Freeborg](https://github.com/JohnFreeborg) ([forum thread](https://www.fordtransitusaforum.com/threads/looking-for-testers-homeassistant-integration-for-velit-a-c.107318/)).

## Supported Devices

| Device | Status | Component |
|--------|--------|-----------|
| Velit 3000R Air Conditioner | ✅ Ready | `velit_ac` |
| Velit Diesel Heater | ✅ Ready | `velit_heater` |

## Architecture

```
You (remote) → VPN → Home Assistant → ESP32 (WiFi) → BLE → Velit Device
```

A single ESP32 board placed within BLE range of your Velit units bridges WiFi to Bluetooth. It connects to both the AC and heater simultaneously and exposes native Home Assistant climate controls, sensors, and buttons. You control everything from the HA dashboard — locally or remotely over VPN.

## Hardware Requirements

- **ESP32-C6** development board (or any ESP32 variant with BLE support)
- USB cable for initial flashing
- Placement within ~30ft / 10m of both Velit units
- Both devices use GATT service `0xFFE0` — one ESP32 handles both

## Quick Start

1. Copy `secrets.yaml.example` to `secrets.yaml` and fill in your WiFi credentials
2. Install ESPHome: `pip install esphome`
3. Update the `mac_address` entries in `velit-controller.yaml` to match your devices
4. Compile and flash: `esphome run velit-controller.yaml`
5. Add the device in Home Assistant

## Finding Your Device MAC Addresses

Use nRF Connect or check ESPHome BLE tracker logs. Velit devices advertise as `VELIT`, `VLIT`, or `D30`. The AC typically has a name like `KT2024XXXXXXXXX`.

## Home Assistant Entities

### Climate — `Velit AC`
- **Modes:** Off, Cool, Heat, Fan Only, Dry
- **Presets:** None, ECO, Sleep, Boost (TURBO)
- **Fan Speeds:** 1–5
- **Swing:** Off, Vertical
- **Temperature Range:** 17–30°C (63–86°F)
- **Current Temperature:** Ambient sensor reading

### Climate — `Velit Heater`
- **Modes:** Off, Heat, Fan Only (ventilation)
- **Presets:** None (Manual), Home (Thermostat)
- **Fan/Gear:** 1–5
- **Temperature Range:** 4–37°C (40–99°F)
- **Current Temperature:** Inlet sensor reading

### AC Supplementary Entities
| Entity | Type | Description |
|--------|------|-------------|
| `Velit AC Swing` | Switch | Swing vane on/off |
| `Velit AC Vent` | Button | Momentary vent toggle |
| `Velit AC Ambient Temperature` | Sensor | Ambient °C |
| `Velit AC BLE Connected` | Binary Sensor | BLE link status |

### Heater Supplementary Entities
| Entity | Type | Description |
|--------|------|-------------|
| `Velit Heater Voltage` | Sensor | Supply voltage (V) |
| `Velit Heater Fan RPM` | Sensor | Fan speed (RPM) |
| `Velit Heater Inlet Temperature` | Sensor | Inlet air temp (°C) |
| `Velit Heater Altitude` | Sensor | Barometric altitude (ft) |
| `Velit Heater Fault` | Text Sensor | Active fault name |
| `Velit Heater State` | Text Sensor | Machine state (Standby/Normal/Cooling Down/etc.) |
| `Velit Heater Ventilation Start/Stop` | Buttons | Fan without combustion |
| `Velit Heater BLE Connected` | Binary Sensor | BLE link status |

### Heater Fault Codes
| Code | Fault | Code | Fault |
|------|-------|------|-------|
| 0 | No Fault | 8 | Fan Fault |
| 1 | Ignition Failure | 9 | Inlet Temp Sensor |
| 2 | Abnormal Flame Out | 10 | Glow Plug |
| 3 | Voltage Deviation | 11 | Ambient Temp Anomaly |
| 4 | Heat Exchanger Temp | 12 | Altitude Out of Range |
| 5 | Ignition Sensor | 13 | Fan Blockage |
| 6 | Outlet Temp Sensor | 14 | CO Exceeded |
| 7 | Fuel Pump | 15 | LIN Communication |

## BLE Protocol

Both devices share the same GATT service (`0xFFE0`) but use completely different application-layer protocols. The AC uses fixed 9-byte frames while the heater uses variable-length frames with device addressing and 16-bit checksums.

See [PROTOCOL.md](PROTOCOL.md) for the complete reverse-engineered specification.

## Project Structure

```
velit-bluetooth/
├── velit-controller.yaml          # Main ESPHome config (both devices)
├── secrets.yaml.example           # WiFi credentials template
├── PROTOCOL.md                    # Full BLE protocol specification
├── README.md
└── components/
    ├── velit_ac/                   # AC custom component
    │   ├── __init__.py
    │   ├── climate.py
    │   ├── switch.py
    │   ├── velit_ac.h
    │   └── velit_ac.cpp
    └── velit_heater/               # Heater custom component
        ├── __init__.py
        ├── climate.py
        ├── velit_heater.h
        └── velit_heater.cpp
```

## License

MIT
