# Velit Bluetooth — ESPHome BLE Controller

Custom ESPHome component for controlling [Velit](https://velitcamping.com) camping HVAC products via Bluetooth Low Energy, enabling remote control through Home Assistant over WiFi/VPN.

## Supported Devices

| Device | Status | Component |
|--------|--------|-----------|
| Velit 3000R Air Conditioner | ✅ Working | `velit_ac` |
| Velit Diesel Heater | 🚧 In Progress | `velit_heater` |

## Architecture

```
You (remote) → VPN → Home Assistant → ESP32 (WiFi) → BLE → Velit Device
```

An ESP32 board placed within BLE range of your Velit unit acts as a transparent bridge between your WiFi network and the Velit's Bluetooth interface. Home Assistant discovers the ESPHome device and exposes native climate controls.

## Hardware Requirements

- ESP32-C6 development board (or any ESP32 variant with BLE support)
- USB cable for initial flashing
- Placement within ~30ft / 10m of the Velit unit

## Quick Start

1. Copy `secrets.yaml.example` to `secrets.yaml` and fill in your WiFi credentials
2. Install ESPHome: `pip install esphome`
3. Update the `mac_address` in `velit-controller.yaml` to match your device
4. Compile and flash: `esphome run velit-controller.yaml`
5. Add the device in Home Assistant

## Home Assistant Entities

### Climate Entity — `Velit AC`
- **Modes:** Off, Cool, Heat, Fan Only
- **Presets:** None (AUTO), ECO, Sleep, Boost (TURBO)
- **Fan Speeds:** 1–5
- **Swing:** Off, Vertical
- **Temperature Range:** 61–86°F (16–30°C)
- **Current Temperature:** Ambient sensor reading

### Supplementary Entities
- **Switch:** `Velit AC Swing` — swing vane on/off
- **Button:** `Velit AC Vent` — momentary vent toggle
- **Sensor:** `Velit AC Ambient Temperature` — ambient °C
- **Binary Sensor:** `Velit AC BLE Connected` — BLE link status

## BLE Protocol

The Velit devices use a simple register-based serial-over-BLE protocol on GATT service `0xFFE0`. See [PROTOCOL.md](PROTOCOL.md) for the full reverse-engineered specification.

## Finding Your Device MAC Address

Use nRF Connect or the ESPHome BLE tracker logs to find your Velit device. It advertises with a name like `KT2024XXXXXXXXX`.

## License

MIT
