#include "velit_ac.h"
#include "esphome/core/log.h"

namespace esphome {
namespace velit_ac {

static const char *const TAG = "velit_ac";

// ============================================================================
// VelitAC — Core BLE protocol handler
// ============================================================================

void VelitAC::setup() {
  ESP_LOGCONFIG(TAG, "Setting up Velit AC BLE controller...");
}

void VelitAC::dump_config() {
  ESP_LOGCONFIG(TAG, "Velit AC BLE Controller:");
  ESP_LOGCONFIG(TAG, "  Write Handle: 0x%04X", this->write_handle_);
  ESP_LOGCONFIG(TAG, "  Notify Handle: 0x%04X", this->notify_handle_);
}

void VelitAC::update() {
  // Called every 6s by PollingComponent — kicks off a new polling cycle
  if (!this->handles_discovered_ || !this->notifications_enabled_) {
    return;
  }
  this->poll_index_ = 0;
  this->last_poll_ms_ = 0;  // Force immediate first query
}

void VelitAC::loop() {
  uint32_t now = millis();

  // Process command queue (prioritize user commands over polling)
  if (!this->command_queue_.empty() &&
      (now - this->last_command_ms_) >= COMMAND_THROTTLE_MS) {
    auto cmd = this->command_queue_.front();
    this->command_queue_.erase(this->command_queue_.begin());
    this->send_command_(cmd.reg, cmd.value);
    this->last_command_ms_ = now;
    return;  // Don't also poll on the same loop iteration
  }

  // Process polling cycle: send one register query per POLL_INTERVAL_MS
  if (this->poll_index_ < POLL_REGISTER_COUNT &&
      this->handles_discovered_ &&
      this->notifications_enabled_ &&
      this->command_queue_.empty() &&
      (now - this->last_poll_ms_) >= POLL_INTERVAL_MS) {
    uint8_t reg = POLL_REGISTERS[this->poll_index_];
    this->send_query_(reg);
    this->poll_index_++;
    this->last_poll_ms_ = now;
  }
}

void VelitAC::gattc_event_handler(esp_gattc_cb_event_t event,
                                   esp_gatt_if_t gattc_if,
                                   esp_ble_gattc_cb_param_t *param) {
  switch (event) {
    case ESP_GATTC_DISCONNECT_EVT: {
      ESP_LOGW(TAG, "Disconnected from Velit AC");
      this->handles_discovered_ = false;
      this->notifications_enabled_ = false;
      this->state_received_ = false;
      break;
    }

    case ESP_GATTC_SEARCH_CMPL_EVT: {
      // Service discovery complete — find our characteristic handles
      auto *write_chr = this->parent_->get_characteristic(VELIT_SERVICE_UUID, VELIT_WRITE_CHAR_UUID);
      if (write_chr == nullptr) {
        ESP_LOGE(TAG, "Could not find write characteristic (FFE2) on device");
        break;
      }
      this->write_handle_ = write_chr->handle;
      ESP_LOGI(TAG, "Found write characteristic FFE2 at handle 0x%04X", this->write_handle_);

      auto *notify_chr = this->parent_->get_characteristic(VELIT_SERVICE_UUID, VELIT_NOTIFY_CHAR_UUID);
      if (notify_chr == nullptr) {
        ESP_LOGE(TAG, "Could not find notify characteristic (FFE1) on device");
        break;
      }
      this->notify_handle_ = notify_chr->handle;
      ESP_LOGI(TAG, "Found notify characteristic FFE1 at handle 0x%04X", this->notify_handle_);

      this->handles_discovered_ = true;

      // Register for notifications on FFE1
      auto status = esp_ble_gattc_register_for_notify(
          this->parent_->get_gattc_if(),
          this->parent_->get_remote_bda(),
          this->notify_handle_);
      if (status != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register for notifications: %d", status);
      } else {
        ESP_LOGI(TAG, "Registered for notifications on FFE1");
      }
      break;
    }

    case ESP_GATTC_REG_FOR_NOTIFY_EVT: {
      if (param->reg_for_notify.status == ESP_GATT_OK) {
        this->notifications_enabled_ = true;
        ESP_LOGI(TAG, "Notifications enabled — starting polling");
        // Kick off first poll cycle immediately
        this->poll_index_ = 0;
        this->last_poll_ms_ = 0;
      } else {
        ESP_LOGE(TAG, "Notification registration failed: %d", param->reg_for_notify.status);
      }
      break;
    }

    case ESP_GATTC_NOTIFY_EVT: {
      if (param->notify.handle == this->notify_handle_) {
        this->handle_notification_(param->notify.value, param->notify.value_len);
      }
      break;
    }

    default:
      break;
  }
}

void VelitAC::send_command_(uint8_t reg, uint8_t value) {
  if (!this->handles_discovered_) {
    ESP_LOGW(TAG, "Cannot send command — not connected");
    return;
  }

  uint8_t frame[FRAME_SIZE];
  frame[0] = FRAME_HEADER_1;   // 0x5A
  frame[1] = FRAME_HEADER_2;   // 0x5A
  frame[2] = FRAME_LENGTH;     // 0x06
  frame[3] = FRAME_CMD;        // 0x01
  frame[4] = reg;
  frame[5] = value;

  // Checksum: lower byte of sum of all preceding bytes
  uint8_t checksum = 0;
  for (int i = 0; i < 6; i++) {
    checksum += frame[i];
  }
  frame[6] = checksum;

  frame[7] = FRAME_TERM_CR;    // 0x0D
  frame[8] = FRAME_TERM_LF;    // 0x0A

  ESP_LOGD(TAG, "TX: reg=0x%02X val=0x%02X chk=0x%02X %s",
           reg, value, checksum, value == 0 ? "(query)" : "(SET)");

  auto status = esp_ble_gattc_write_char(
      this->parent_->get_gattc_if(),
      this->parent_->get_conn_id(),
      this->write_handle_,
      FRAME_SIZE,
      frame,
      ESP_GATT_WRITE_TYPE_RSP,
      ESP_GATT_AUTH_REQ_NONE);

  if (status != ESP_OK) {
    ESP_LOGE(TAG, "BLE write failed: %d", status);
  }
}

void VelitAC::handle_notification_(const uint8_t *data, uint16_t length) {
  // Validate frame: 5A 5A 06 01 REG VAL CHK 0D 0A
  if (length != FRAME_SIZE) {
    ESP_LOGW(TAG, "Unexpected notification length: %d (expected %d)", length, FRAME_SIZE);
    return;
  }
  if (data[0] != FRAME_HEADER_1 || data[1] != FRAME_HEADER_2) {
    ESP_LOGW(TAG, "Invalid frame header: 0x%02X 0x%02X", data[0], data[1]);
    return;
  }
  if (data[7] != FRAME_TERM_CR || data[8] != FRAME_TERM_LF) {
    ESP_LOGW(TAG, "Invalid frame terminator");
    return;
  }

  // Verify checksum
  uint8_t expected_checksum = 0;
  for (int i = 0; i < 6; i++) {
    expected_checksum += data[i];
  }
  if (data[6] != expected_checksum) {
    ESP_LOGW(TAG, "Checksum mismatch: got 0x%02X expected 0x%02X", data[6], expected_checksum);
    return;
  }

  uint8_t reg = data[4];
  uint8_t val = data[5];

  ESP_LOGD(TAG, "RX: reg=0x%02X val=0x%02X (%d)", reg, val, val);

  bool changed = false;

  switch (reg) {
    case REG_POWER:
      if (this->power_state_ != val) {
        this->power_state_ = val;
        changed = true;
        ESP_LOGI(TAG, "Power: %s", val == POWER_ON ? "ON" : "OFF");
      }
      break;

    case REG_MODE:
      if (this->mode_state_ != val) {
        this->mode_state_ = val;
        changed = true;
        ESP_LOGI(TAG, "Mode: 0x%02X", val);
      }
      break;

    case REG_SETPOINT:
      if (this->setpoint_f_ != val) {
        this->setpoint_f_ = val;
        changed = true;
        ESP_LOGI(TAG, "Set point: %d°F", val);
      }
      break;

    case REG_FAN_SPEED:
      if (this->fan_speed_ != val) {
        this->fan_speed_ = val;
        changed = true;
        ESP_LOGI(TAG, "Fan speed: %d", val);
      }
      break;

    case REG_AMBIENT: {
      float new_ambient = static_cast<float>(val);
      if (this->ambient_c_ != new_ambient) {
        this->ambient_c_ = new_ambient;
        changed = true;
        // Convert °C to °F for logging (HA handles display unit)
        float ambient_f = (new_ambient * 9.0f / 5.0f) + 32.0f;
        ESP_LOGI(TAG, "Ambient: %.0f°C (%.0f°F)", new_ambient, ambient_f);
      }
      break;
    }

    case REG_STATUS:
      if (this->status_ != val) {
        this->status_ = val;
        ESP_LOGI(TAG, "Status: 0x%02X", val);
      }
      break;

    case REG_SWING:
      if (this->swing_state_ != val) {
        this->swing_state_ = val;
        changed = true;
        ESP_LOGI(TAG, "Swing: %s", val == SWING_ON ? "ON" : "OFF");
      }
      break;

    default:
      ESP_LOGD(TAG, "Unknown register 0x%02X = 0x%02X", reg, val);
      break;
  }

  this->state_received_ = true;

  if (changed) {
    this->publish_state_();
  }
}

void VelitAC::publish_state_() {
  if (this->climate_ != nullptr) {
    // --- Power / Mode ---
    if (this->power_state_ != POWER_ON) {
      this->climate_->mode = climate::CLIMATE_MODE_OFF;
      this->climate_->preset.reset();
    } else {
      // Map Velit mode register to HA climate mode + preset
      switch (this->mode_state_) {
        case MODE_COOL_AUTO:
          this->climate_->mode = climate::CLIMATE_MODE_COOL;
          this->climate_->preset = climate::CLIMATE_PRESET_NONE;
          break;
        case MODE_HEAT:
          this->climate_->mode = climate::CLIMATE_MODE_HEAT;
          this->climate_->preset = climate::CLIMATE_PRESET_NONE;
          break;
        case MODE_FAN:
          this->climate_->mode = climate::CLIMATE_MODE_FAN_ONLY;
          this->climate_->preset = climate::CLIMATE_PRESET_NONE;
          break;
        case MODE_ECO:
          this->climate_->mode = climate::CLIMATE_MODE_COOL;
          this->climate_->preset = climate::CLIMATE_PRESET_ECO;
          break;
        case MODE_SLEEP:
          this->climate_->mode = climate::CLIMATE_MODE_COOL;
          this->climate_->preset = climate::CLIMATE_PRESET_SLEEP;
          break;
        case MODE_TURBO:
          this->climate_->mode = climate::CLIMATE_MODE_COOL;
          this->climate_->preset = climate::CLIMATE_PRESET_BOOST;
          break;
        default:
          this->climate_->mode = climate::CLIMATE_MODE_COOL;
          this->climate_->preset = climate::CLIMATE_PRESET_NONE;
          break;
      }
    }

    // --- Temperatures ---
    // Set point: stored in °F, HA climate uses °C internally
    this->climate_->target_temperature = (static_cast<float>(this->setpoint_f_) - 32.0f) * 5.0f / 9.0f;
    // Ambient: already in °C
    this->climate_->current_temperature = this->ambient_c_;

    // --- Fan mode ---
    switch (this->fan_speed_) {
      case 1: this->climate_->custom_fan_mode = "1"; break;
      case 2: this->climate_->custom_fan_mode = "2"; break;
      case 3: this->climate_->custom_fan_mode = "3"; break;
      case 4: this->climate_->custom_fan_mode = "4"; break;
      case 5: this->climate_->custom_fan_mode = "5"; break;
      default: this->climate_->custom_fan_mode = "Auto"; break;
    }

    // --- Swing ---
    this->climate_->swing_mode = (this->swing_state_ == SWING_ON)
        ? climate::CLIMATE_SWING_VERTICAL
        : climate::CLIMATE_SWING_OFF;

    this->climate_->publish_state();
  }

  // Update swing switch
  if (this->swing_switch_ != nullptr) {
    this->swing_switch_->publish_state(this->swing_state_ == SWING_ON);
  }
}

// --- Control methods (called by climate/switch entities) ---

void VelitAC::set_power(bool on) {
  uint8_t val = on ? POWER_ON : POWER_OFF;
  ESP_LOGI(TAG, "Setting power: %s", on ? "ON" : "OFF");
  this->command_queue_.push_back({REG_POWER, val});
}

void VelitAC::set_mode(uint8_t mode) {
  ESP_LOGI(TAG, "Setting mode: 0x%02X", mode);
  this->command_queue_.push_back({REG_MODE, mode});
}

void VelitAC::set_target_temp_f(uint8_t temp_f) {
  if (temp_f < TEMP_MIN_F) temp_f = TEMP_MIN_F;
  if (temp_f > TEMP_MAX_F) temp_f = TEMP_MAX_F;
  ESP_LOGI(TAG, "Setting target temp: %d°F", temp_f);
  this->command_queue_.push_back({REG_SETPOINT, temp_f});
}

void VelitAC::set_fan_speed(uint8_t speed) {
  if (speed < 1) speed = 1;
  if (speed > 5) speed = 5;
  ESP_LOGI(TAG, "Setting fan speed: %d", speed);
  this->command_queue_.push_back({REG_FAN_SPEED, speed});
}

void VelitAC::set_swing(bool on) {
  uint8_t val = on ? SWING_ON : SWING_OFF;
  ESP_LOGI(TAG, "Setting swing: %s", on ? "ON" : "OFF");
  this->command_queue_.push_back({REG_SWING, val});
}

void VelitAC::vent_toggle() {
  ESP_LOGI(TAG, "Toggling vent (momentary)");
  this->command_queue_.push_back({REG_MODE, MODE_VENT_TOGGLE});
}


// ============================================================================
// VelitACClimate — HA Climate entity
// ============================================================================

void VelitACClimate::dump_config() {
  ESP_LOGCONFIG(TAG, "Velit AC Climate Entity");
}

climate::ClimateTraits VelitACClimate::traits() {
  auto traits = climate::ClimateTraits();

  traits.set_supports_current_temperature(true);
  traits.set_visual_min_temperature(16.1f);   // 61°F in °C
  traits.set_visual_max_temperature(30.0f);   // 86°F in °C
  traits.set_visual_temperature_step(0.5f);   // ~1°F steps

  // Supported modes
  traits.set_supported_modes({
      climate::CLIMATE_MODE_OFF,
      climate::CLIMATE_MODE_COOL,
      climate::CLIMATE_MODE_HEAT,
      climate::CLIMATE_MODE_FAN_ONLY,
  });

  // Supported presets (ECO, SLEEP, TURBO mapped to BOOST)
  traits.set_supported_presets({
      climate::CLIMATE_PRESET_NONE,
      climate::CLIMATE_PRESET_ECO,
      climate::CLIMATE_PRESET_SLEEP,
      climate::CLIMATE_PRESET_BOOST,
  });

  // Fan speeds 1-5
  traits.set_supported_custom_fan_modes({
      "1", "2", "3", "4", "5"
  });

  // Swing support
  traits.set_supported_swing_modes({
      climate::CLIMATE_SWING_OFF,
      climate::CLIMATE_SWING_VERTICAL,
  });

  return traits;
}

void VelitACClimate::control(const climate::ClimateCall &call) {
  if (this->parent_ == nullptr) return;

  // Handle mode changes
  if (call.get_mode().has_value()) {
    auto mode = *call.get_mode();
    switch (mode) {
      case climate::CLIMATE_MODE_OFF:
        this->parent_->set_power(false);
        break;
      case climate::CLIMATE_MODE_COOL:
        if (!this->parent_->is_powered_on()) {
          this->parent_->set_power(true);
        }
        this->parent_->set_mode(MODE_COOL_AUTO);
        break;
      case climate::CLIMATE_MODE_HEAT:
        if (!this->parent_->is_powered_on()) {
          this->parent_->set_power(true);
        }
        this->parent_->set_mode(MODE_HEAT);
        break;
      case climate::CLIMATE_MODE_FAN_ONLY:
        if (!this->parent_->is_powered_on()) {
          this->parent_->set_power(true);
        }
        this->parent_->set_mode(MODE_FAN);
        break;
      default:
        break;
    }
  }

  // Handle preset changes
  if (call.get_preset().has_value()) {
    auto preset = *call.get_preset();
    // Ensure powered on for presets
    if (!this->parent_->is_powered_on()) {
      this->parent_->set_power(true);
    }
    switch (preset) {
      case climate::CLIMATE_PRESET_NONE:
        this->parent_->set_mode(MODE_COOL_AUTO);
        break;
      case climate::CLIMATE_PRESET_ECO:
        this->parent_->set_mode(MODE_ECO);
        break;
      case climate::CLIMATE_PRESET_SLEEP:
        this->parent_->set_mode(MODE_SLEEP);
        break;
      case climate::CLIMATE_PRESET_BOOST:
        this->parent_->set_mode(MODE_TURBO);
        break;
      default:
        break;
    }
  }

  // Handle target temperature
  if (call.get_target_temperature().has_value()) {
    float temp_c = *call.get_target_temperature();
    // Convert °C back to °F for the device
    uint8_t temp_f = static_cast<uint8_t>(roundf((temp_c * 9.0f / 5.0f) + 32.0f));
    this->parent_->set_target_temp_f(temp_f);
  }

  // Handle custom fan mode
  if (call.get_custom_fan_mode().has_value()) {
    auto fan_mode = *call.get_custom_fan_mode();
    uint8_t speed = 0;
    if (fan_mode == "1") speed = 1;
    else if (fan_mode == "2") speed = 2;
    else if (fan_mode == "3") speed = 3;
    else if (fan_mode == "4") speed = 4;
    else if (fan_mode == "5") speed = 5;
    if (speed > 0) {
      this->parent_->set_fan_speed(speed);
    }
  }

  // Handle swing mode
  if (call.get_swing_mode().has_value()) {
    auto swing = *call.get_swing_mode();
    this->parent_->set_swing(swing == climate::CLIMATE_SWING_VERTICAL);
  }
}


// ============================================================================
// VelitSwingSwitch — Standalone swing toggle switch
// ============================================================================

void VelitSwingSwitch::dump_config() {
  ESP_LOGCONFIG(TAG, "Velit Swing Switch");
}

void VelitSwingSwitch::write_state(bool state) {
  if (this->parent_ != nullptr) {
    this->parent_->set_swing(state);
  }
  // Don't publish here — wait for device confirmation via notification
}

}  // namespace velit_ac
}  // namespace esphome
