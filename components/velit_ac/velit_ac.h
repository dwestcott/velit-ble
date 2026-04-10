#pragma once

#include "esphome/core/component.h"
#include "esphome/components/ble_client/ble_client.h"
#include "esphome/components/climate/climate.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/switch/switch.h"

namespace esphome {
namespace velit_ac {

// Velit BLE Protocol Constants
static const uint16_t VELIT_SERVICE_UUID = 0xFFE0;
static const uint16_t VELIT_WRITE_CHAR_UUID = 0xFFE2;
static const uint16_t VELIT_NOTIFY_CHAR_UUID = 0xFFE1;

// Frame constants
static const uint8_t FRAME_HEADER_1 = 0x5A;
static const uint8_t FRAME_HEADER_2 = 0x5A;
static const uint8_t FRAME_LENGTH = 0x06;
static const uint8_t FRAME_CMD = 0x01;
static const uint8_t FRAME_TERM_CR = 0x0D;
static const uint8_t FRAME_TERM_LF = 0x0A;
static const uint8_t FRAME_SIZE = 9;

// Register addresses
static const uint8_t REG_POWER = 0x01;
static const uint8_t REG_MODE = 0x02;
static const uint8_t REG_SETPOINT = 0x03;
static const uint8_t REG_FAN_SPEED = 0x04;
static const uint8_t REG_AMBIENT = 0x07;
static const uint8_t REG_STATUS = 0x0B;
static const uint8_t REG_SWING = 0x10;

// Power values
static const uint8_t POWER_OFF = 0x01;
static const uint8_t POWER_ON = 0x02;

// Mode values (register 0x02)
static const uint8_t MODE_COOL = 0x01;
static const uint8_t MODE_HEAT = 0x02;
static const uint8_t MODE_FAN = 0x03;
static const uint8_t MODE_ECO = 0x04;
static const uint8_t MODE_SLEEP = 0x05;
static const uint8_t MODE_TURBO = 0x06;
static const uint8_t MODE_DRY = 0x07;
static const uint8_t MODE_VENT = 0x08;

// Swing values
static const uint8_t SWING_OFF = 0x01;
static const uint8_t SWING_ON = 0x02;

// Temperature limits
// Celsius: 17-30°C, Fahrenheit: 63-86°F
// Device stores in whichever unit is active on its display
static const float TEMP_MIN_C = 17.0f;
static const float TEMP_MAX_C = 30.0f;
static const uint8_t TEMP_MIN_F = 63;
static const uint8_t TEMP_MAX_F = 86;

// Command timing (from community integration)
static const uint32_t AC_COMMAND_INTERVAL_MS = 400;
static const uint32_t AC_RESPONSE_TIMEOUT_MS = 3000;

// Polling registers in order
static const uint8_t POLL_REGISTERS[] = {
    REG_POWER, REG_MODE, REG_SETPOINT, REG_FAN_SPEED,
    REG_AMBIENT, REG_STATUS, REG_SWING
};
static const uint8_t POLL_REGISTER_COUNT = 7;

class VelitACClimate;
class VelitSwingSwitch;

class VelitAC : public PollingComponent, public ble_client::BLEClientNode {
 public:
  void setup() override;
  void update() override;
  void dump_config() override;
  void loop() override;

  void gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                           esp_ble_gattc_cb_param_t *param) override;

  float get_setup_priority() const override { return setup_priority::AFTER_BLUETOOTH; }

  // Entity setters (called from Python config)
  void set_climate(VelitACClimate *climate) { this->climate_ = climate; }
  void set_swing_switch(VelitSwingSwitch *sw) { this->swing_switch_ = sw; }

  // Control methods (called from climate/switch entities)
  void set_power(bool on);
  void set_mode(uint8_t mode);
  void set_target_temp_f(uint8_t temp_f);
  void set_fan_speed(uint8_t speed);
  void set_swing(bool on);
  void vent_toggle();

  // State getters
  bool is_powered_on() const { return power_state_ == POWER_ON; }
  uint8_t get_mode() const { return mode_state_; }
  uint8_t get_target_temp_f() const { return setpoint_f_; }
  uint8_t get_fan_speed() const { return fan_speed_; }
  float get_ambient_temp_c() const { return ambient_c_; }
  bool is_swing_on() const { return swing_state_ == SWING_ON; }

 protected:
  // Build and send a command frame
  void send_command_(uint8_t reg, uint8_t value);

  // Send a read query for a register
  void send_query_(uint8_t reg) { send_command_(reg, 0x00); }

  // Parse a notification frame and update state
  void handle_notification_(const uint8_t *data, uint16_t length);

  // Publish current state to all HA entities
  void publish_state_();

  // BLE handles (discovered at runtime)
  uint16_t write_handle_{0};
  uint16_t notify_handle_{0};
  bool handles_discovered_{false};
  bool notifications_enabled_{false};

  // Polling state
  uint8_t poll_index_{0};
  uint32_t last_poll_ms_{0};
  static const uint32_t POLL_INTERVAL_MS = 600;  // 600ms between register queries

  // Device state (from notifications)
  uint8_t power_state_{POWER_OFF};
  uint8_t mode_state_{MODE_COOL_AUTO};
  uint8_t setpoint_f_{68};
  uint8_t fan_speed_{2};
  float ambient_c_{0.0f};
  uint8_t status_{0};
  uint8_t swing_state_{SWING_OFF};
  bool state_received_{false};

  // Child entities
  VelitACClimate *climate_{nullptr};
  VelitSwingSwitch *swing_switch_{nullptr};

  // Command queue (to avoid BLE write collisions)
  struct PendingCommand {
    uint8_t reg;
    uint8_t value;
  };
  std::vector<PendingCommand> command_queue_;
  uint32_t last_command_ms_{0};
  static const uint32_t COMMAND_THROTTLE_MS = 400;  // AC requires 400ms min between commands
};

// Climate entity that delegates to VelitAC
class VelitACClimate : public climate::Climate, public Component {
 public:
  void setup() override {}
  void dump_config() override;

  void set_parent(VelitAC *parent) { this->parent_ = parent; }

  climate::ClimateTraits traits() override;
  void control(const climate::ClimateCall &call) override;

 protected:
  friend class VelitAC;
  VelitAC *parent_{nullptr};
};

// Swing switch that delegates to VelitAC
class VelitSwingSwitch : public switch_::Switch, public Component {
 public:
  void setup() override {}
  void dump_config() override;

  void set_parent(VelitAC *parent) { this->parent_ = parent; }

 protected:
  friend class VelitAC;
  void write_state(bool state) override;
  VelitAC *parent_{nullptr};
};

}  // namespace velit_ac
}  // namespace esphome
