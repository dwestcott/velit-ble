#pragma once

#include "esphome/core/component.h"
#include "esphome/components/ble_client/ble_client.h"
#include "esphome/components/climate/climate.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"

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
  void set_voltage_sensor(sensor::Sensor *s) { this->voltage_sensor_ = s; }
  void set_fan_rpm_sensor(sensor::Sensor *s) { this->fan_rpm_sensor_ = s; }
  void set_inlet_temp_sensor(sensor::Sensor *s) { this->inlet_temp_sensor_ = s; }
  void set_outlet_temp_sensor(sensor::Sensor *s) { this->outlet_temp_sensor_ = s; }
  void set_altitude_sensor(sensor::Sensor *s) { this->altitude_sensor_ = s; }
  void set_fault_sensor(text_sensor::TextSensor *s) { this->fault_sensor_ = s; }
  void set_state_sensor(text_sensor::TextSensor *s) { this->state_sensor_ = s; }

  // Control methods
  void startup_manual();
  void shutdown();
  void set_mode(uint8_t mode);
  void set_gear(uint8_t gear);
  void set_target_temp_f(uint8_t temp_f);
  void start_ventilation();
  void stop_ventilation();
  void start_fuel_prime();
  void stop_fuel_prime();
  void start_cleaning();

  // State getters
  bool is_connected() const { return this->handles_discovered_ && this->notifications_enabled_; }
  bool is_running() const { return machine_state_ == STATE_NORMAL; }
  uint8_t get_mode() const { return work_mode_; }
  uint8_t get_gear() const { return current_gear_; }
  uint8_t get_fault() const { return fault_code_; }
  uint8_t get_machine_state() const { return machine_state_; }
  float get_inlet_temp_c() const;
  float get_voltage() const { return voltage_; }
  uint16_t get_fan_rpm() const { return fan_rpm_; }
  uint16_t get_altitude() const { return altitude_; }

 protected:
  // Build and send a command frame
  void send_command_(uint8_t func, const uint8_t *data, uint8_t data_len);

  // Convenience: send single-byte command
  void send_command_(uint8_t func, uint8_t value);

  // Send a query
  void send_query_(uint8_t func) { send_command_(func, (uint8_t)0x00); }

  // Send clock sync on connect
  void send_clock_sync_();

  // Parse notification response
  void handle_notification_(const uint8_t *data, uint16_t length);

  // Parse specific query responses
  void parse_status_(const uint8_t *data, uint8_t len);   // func 0x0A
  void parse_sensors_(const uint8_t *data, uint8_t len);   // func 0x0B

  // Publish state to HA entities
  void publish_state_();

  // BLE handles
  uint16_t write_handle_{0};
  uint16_t notify_handle_{0};
  bool handles_discovered_{false};
  bool notifications_enabled_{false};
  bool clock_synced_{false};

  // Polling state
  uint8_t poll_phase_{0};
  uint32_t last_poll_ms_{0};
  static const uint32_t POLL_INTERVAL_MS = 800;

  // Device state from Query 1 (0x0A)
  uint8_t fault_code_{0};
  uint8_t work_mode_{MODE_MANUAL};
  uint8_t current_gear_{1};
  uint8_t set_temp_raw_{70};
  uint8_t machine_state_{STATE_STANDBY};
  uint8_t power_watts_{0};
  uint8_t pump_freq_raw_{0};

  // Sensor data from Query 2 (0x0B)
  float voltage_{0.0f};
  uint16_t fan_rpm_{0};
  float inlet_temp_f_{0.0f};
  float casing_temp_f_{0.0f};
  float outlet_temp_f_{0.0f};
  uint16_t altitude_{0};
  bool sensors_valid_{false};

  // Device uses Fahrenheit (detected from setpoint range)
  bool fahrenheit_mode_{true};

  bool state_received_{false};

  // Child entities
  VelitHeaterClimate *climate_{nullptr};
  sensor::Sensor *voltage_sensor_{nullptr};
  sensor::Sensor *fan_rpm_sensor_{nullptr};
  sensor::Sensor *inlet_temp_sensor_{nullptr};
  sensor::Sensor *outlet_temp_sensor_{nullptr};
  sensor::Sensor *altitude_sensor_{nullptr};
  text_sensor::TextSensor *fault_sensor_{nullptr};
  text_sensor::TextSensor *state_sensor_{nullptr};

  // Command queue
  struct PendingCommand {
    uint8_t func;
    std::vector<uint8_t> data;
  };
  std::vector<PendingCommand> command_queue_;
  uint32_t last_command_ms_{0};
  static const uint32_t COMMAND_THROTTLE_MS = 400;
};

// Climate entity for the heater
class VelitHeaterClimate : public climate::Climate, public Component {
 public:
  void setup() override {}
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
