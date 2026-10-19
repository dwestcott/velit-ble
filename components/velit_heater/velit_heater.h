#pragma once

#include "esphome/core/component.h"
#include "esphome/components/ble_client/ble_client.h"
#include "esphome/components/climate/climate.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"

#include <cmath>
#include <string>
#include <vector>

namespace esphome {
namespace velit_heater {

// GATT UUIDs (shared with AC)
static const uint16_t VELIT_SERVICE_UUID = 0xFFE0;
static const uint16_t VELIT_WRITE_CHAR_UUID = 0xFFE2;
static const uint16_t VELIT_NOTIFY_CHAR_UUID = 0xFFE1;

// Heater protocol constants
static const uint8_t CMD_HEADER = 0x55;
static const uint8_t RSP_HEADER = 0xAA;
static const uint8_t MANUF_CODE_S = 0x53;  // "SF"
static const uint8_t MANUF_CODE_F = 0x46;

// Fixed device addressing
static const uint8_t MASTER_ADDR[] = {0x00, 0x00, 0x00, 0x01};
static const uint8_t SLAVE_ADDR[]  = {0x00, 0x00, 0x00, 0x2D};

// Function codes — Control
static const uint8_t FUNC_SET_MODE       = 0x00;
static const uint8_t FUNC_STARTUP        = 0x01;
static const uint8_t FUNC_SHUTDOWN       = 0x02;
static const uint8_t FUNC_START_VENT     = 0x03;
static const uint8_t FUNC_STOP_VENT      = 0x04;
static const uint8_t FUNC_START_PRIME    = 0x05;
static const uint8_t FUNC_STOP_PRIME     = 0x06;
static const uint8_t FUNC_SET_GEAR       = 0x07;
static const uint8_t FUNC_SET_TEMP       = 0x08;
static const uint8_t FUNC_CLEANING       = 0x09;

// Function codes — Queries
static const uint8_t FUNC_QUERY_STATUS   = 0x0A;
static const uint8_t FUNC_QUERY_SENSORS  = 0x0B;
static const uint8_t FUNC_AUTO_START_CFG = 0x0E;
static const uint8_t FUNC_QUERY_TIMER    = 0x0F;
static const uint8_t FUNC_CLOCK_SYNC     = 0x10;
static const uint8_t FUNC_FIRMWARE       = 0x6A;  // data [0x01]; version at payload[14..15]

// Mode values
static const uint8_t MODE_MANUAL     = 0x01;
static const uint8_t MODE_THERMOSTAT = 0x02;

// Machine states
static const uint8_t STATE_STANDBY        = 0;
static const uint8_t STATE_NORMAL         = 1;
static const uint8_t STATE_COOLING_DOWN   = 2;
static const uint8_t STATE_OVERTEMP       = 3;
static const uint8_t STATE_CLEANING       = 4;
static const uint8_t STATE_CLEAN_COMPLETE = 5;

// Temperature limits (°F mode)
static const uint8_t HEATER_TEMP_MIN_F = 40;
static const uint8_t HEATER_TEMP_MAX_F = 99;
// Temperature limits (°C mode)
static const float HEATER_TEMP_MIN_C = 4.0f;
static const float HEATER_TEMP_MAX_C = 37.0f;

// Plausible altitude ceilings (above Everest). Higher readings are startup
// glitches; the last good value is held instead.
static const uint16_t MAX_ALTITUDE_FT = 30000;
static const uint16_t MAX_ALTITUDE_M = 9000;

// No valid reply for this long = data is stale; getters return NAN so HA
// shows the sensors as unknown instead of the last value forever.
static const uint32_t STALE_TIMEOUT_MS = 60000;
// Poll at the fast rate (update_interval) for this long after any command.
static const uint32_t FAST_POLL_AFTER_COMMAND_MS = 60000;

// Sensor unavailable marker
static const uint16_t SENSOR_UNAVAILABLE = 0xFFFF;

// Minimum valid response length
static const uint8_t MIN_RESPONSE_LEN = 16;

// Fault code names
static const char *const FAULT_NAMES[] = {
    "No Fault",                    // 0
    "Ignition Failure",            // 1
    "Abnormal Flame Out",          // 2
    "Voltage Deviation",           // 3
    "Heat Exchanger Temp Anomaly", // 4
    "Ignition Sensor Fault",       // 5
    "Outlet Temp Sensor Fault",    // 6
    "Fuel Pump Fault",             // 7
    "Fan Fault",                   // 8
    "Inlet Temp Sensor Fault",     // 9
    "Glow Plug Fault",             // 10
    "Ambient Temp Anomaly",        // 11
    "Altitude Out of Range",       // 12
    "Fan Blockage Fault",          // 13
    "CO Pollution Exceeded",       // 14
    "LIN Communication Fault",     // 15
};
static const uint8_t FAULT_NAME_COUNT = 16;

static const char *const STATE_NAMES[] = {
    "Standby", "Normal", "Cooling Down",
    "Overtemp Standby", "Cleaning", "Clean Complete"
};
static const uint8_t STATE_NAME_COUNT = 6;

class VelitHeaterClimate;

class VelitHeater : public PollingComponent, public ble_client::BLEClientNode {
 public:
  void setup() override;
  void update() override;
  void dump_config() override;
  void loop() override;

  void gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                           esp_ble_gattc_cb_param_t *param) override;

  float get_setup_priority() const override { return setup_priority::AFTER_BLUETOOTH; }

  // Entity setters (implemented in .cpp)
  void set_climate(VelitHeaterClimate *climate);
  // Settled states (Standby, Normal, Overtemp Standby) poll at this slower
  // rate; transitions and the minute after a command use update_interval.
  void set_idle_interval(uint32_t ms) { this->idle_interval_ms_ = ms; }

  // Control methods
  void startup();         // starts in the heater's current mode (Manual/Thermostat)
  void startup_manual();  // kept for existing YAML lambdas; same as startup()
  void shutdown();
  void set_mode(uint8_t mode);
  void set_gear(uint8_t gear);
  void set_target_temp_f(uint8_t temp_f);
  void set_target_temp_c(float temp_c);  // converts to the heater's unit
  void start_ventilation();
  void stop_ventilation();
  void start_fuel_prime();
  void stop_fuel_prime();
  void start_cleaning();

  // State getters
  bool is_connected() const { return this->handles_discovered_ && this->notifications_enabled_; }
  bool is_running() const { return machine_state_ == STATE_NORMAL; }
  bool has_fresh_data() const;
  uint8_t get_mode() const { return work_mode_; }
  uint8_t get_gear() const { return current_gear_; }
  uint8_t get_fault() const { return fault_code_; }
  uint8_t get_machine_state() const { return machine_state_; }
  // Numeric getters return NAN when the data is stale (see STALE_TIMEOUT_MS)
  float get_inlet_temp_c() const;
  float get_voltage() const;
  float get_fan_rpm() const;
  float get_altitude() const;
  bool is_fahrenheit() const { return fahrenheit_mode_; }
  // Status bytes 5-6; meaning unconfirmed (protocol doc: watts, pump Hz x10)
  uint8_t get_power_raw() const { return power_watts_; }
  uint8_t get_pump_raw() const { return pump_freq_raw_; }
  // Heater controller firmware, e.g. "3.13"; empty until a 0x6A reply with a
  // decodable version arrives
  const std::string &get_firmware_version() const { return firmware_version_; }
  // Raw 0x6A reply payload as hex; empty until it arrives
  const std::string &get_firmware_reply() const { return firmware_reply_; }

 protected:
  // Build and send a command frame
  void send_command_(uint8_t func, const uint8_t *data, uint8_t data_len);

  // Convenience: send a one-byte data command
  void send_command_(uint8_t func, uint8_t value);

  // Send a query
  void send_query_(uint8_t func) { send_command_(func, (uint8_t)0x00); }

  // Send clock sync on connect
  bool send_clock_sync_();

  // Parse notification response
  void handle_notification_(const uint8_t *data, uint16_t length);

  // Parse specific query responses
  void parse_status_(const uint8_t *data, uint8_t len);   // func 0x0A
  void parse_sensors_(const uint8_t *data, uint8_t len);   // func 0x0B

  // Publish the climate entity (only when something HA shows has changed)
  void publish_state_();

  // Raw temperature reading -> °C, honouring the heater's unit
  float raw_temp_to_c_(uint16_t raw) const;

  // BLE handles
  uint16_t write_handle_{0};
  uint16_t notify_handle_{0};
  bool handles_discovered_{false};
  bool notifications_enabled_{false};
  bool clock_synced_{false};
  bool firmware_requested_{false};
  uint8_t firmware_tries_{0};
  uint32_t firmware_query_ms_{0};
  std::string firmware_version_;
  std::string firmware_reply_;

  // Polling state
  uint8_t poll_phase_{2};  // 2 = idle; update() resets to 0 to start a cycle
  uint32_t last_poll_ms_{0};
  uint32_t last_cycle_ms_{0};
  uint32_t last_valid_rx_ms_{0};
  uint32_t idle_interval_ms_{15000};
  static const uint32_t POLL_INTERVAL_MS = 800;

  // Device state from Query 1 (0x0A)
  uint8_t fault_code_{0};
  uint8_t work_mode_{MODE_MANUAL};
  uint8_t current_gear_{1};
  uint8_t set_temp_raw_{70};
  uint8_t machine_state_{STATE_STANDBY};
  uint8_t power_watts_{0};
  uint8_t pump_freq_raw_{0};

  // Sensor data from Query 2 (0x0B); NAN until first valid reading
  float voltage_{NAN};
  float fan_rpm_{NAN};
  float inlet_temp_c_{NAN};
  float altitude_{NAN};
  bool sensors_valid_{false};

  // Device uses Fahrenheit (detected from setpoint range)
  bool fahrenheit_mode_{true};

  bool state_received_{false};
  bool published_once_{false};
  uint8_t published_gear_{0};

  // Child entities
  VelitHeaterClimate *climate_{nullptr};

  // Command queue
  struct PendingCommand {
    uint8_t func;
    std::vector<uint8_t> data;
  };
  std::vector<PendingCommand> command_queue_;
  uint32_t last_command_ms_{0};
  uint32_t last_user_command_ms_{0};
  void queue_command_(uint8_t func, uint8_t value);
  static const uint32_t COMMAND_THROTTLE_MS = 400;
};

// Climate entity for the heater
class VelitHeaterClimate : public climate::Climate, public Component {
 public:
  // Register supported custom fan modes here (rather than in traits()) per
  // ESPHome 2026.11+ API: set_supported_custom_fan_modes() lives on the
  // Climate entity, ClimateTraits just references the stored list.
  void setup() override {
    this->set_supported_custom_fan_modes({"1", "2", "3", "4", "5"});
  }
  void dump_config() override;

  void set_parent(VelitHeater *parent) { this->parent_ = parent; }

  climate::ClimateTraits traits() override;
  void control(const climate::ClimateCall &call) override;

 protected:
  friend class VelitHeater;
  VelitHeater *parent_{nullptr};
};

}  // namespace velit_heater
}  // namespace esphome
