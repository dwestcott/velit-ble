#include "velit_heater.h"
#include "esphome/core/log.h"

#include <ctime>

namespace esphome {
namespace velit_heater {

static const char *const TAG = "velit_heater";

// ============================================================================
// VelitHeater — Core BLE protocol handler
// ============================================================================

void VelitHeater::setup() {
  ESP_LOGCONFIG(TAG, "Setting up Velit Heater BLE controller...");
}

void VelitHeater::set_climate(VelitHeaterClimate *climate) {
  this->climate_ = climate;
  climate->set_parent(this);
}

void VelitHeater::dump_config() {
  ESP_LOGCONFIG(TAG, "Velit Heater BLE Controller:");
  ESP_LOGCONFIG(TAG, "  Write Handle: 0x%04X", this->write_handle_);
  ESP_LOGCONFIG(TAG, "  Notify Handle: 0x%04X", this->notify_handle_);
}

void VelitHeater::update() {
  // Called by PollingComponent — kicks off a new poll cycle
  if (!this->handles_discovered_ || !this->notifications_enabled_)
    return;
  this->poll_phase_ = 0;
  this->last_poll_ms_ = 0;
}

void VelitHeater::loop() {
  uint32_t now = millis();

  // Process command queue first
  if (!this->command_queue_.empty() &&
      (now - this->last_command_ms_) >= COMMAND_THROTTLE_MS) {
    auto &cmd = this->command_queue_.front();
    this->send_command_(cmd.func, cmd.data.data(), cmd.data.size());
    this->command_queue_.erase(this->command_queue_.begin());
    this->last_command_ms_ = now;
    return;
  }

  // Poll cycle: query status (0x0A) then sensors (0x0B)
  if (this->poll_phase_ < 2 &&
      this->handles_discovered_ &&
      this->notifications_enabled_ &&
      this->command_queue_.empty() &&
      (now - this->last_poll_ms_) >= POLL_INTERVAL_MS) {

    if (this->poll_phase_ == 0) {
      this->send_query_(FUNC_QUERY_STATUS);
    } else if (this->poll_phase_ == 1) {
      this->send_query_(FUNC_QUERY_SENSORS);
    }
    this->poll_phase_++;
    this->last_poll_ms_ = now;
  }
}

void VelitHeater::gattc_event_handler(esp_gattc_cb_event_t event,
                                       esp_gatt_if_t gattc_if,
                                       esp_ble_gattc_cb_param_t *param) {
  switch (event) {
    case ESP_GATTC_DISCONNECT_EVT: {
      ESP_LOGW(TAG, "Disconnected from Velit Heater");
      this->handles_discovered_ = false;
      this->notifications_enabled_ = false;
      this->clock_synced_ = false;
      this->state_received_ = false;
      break;
    }

    case ESP_GATTC_SEARCH_CMPL_EVT: {
      auto *write_chr = this->parent_->get_characteristic(VELIT_SERVICE_UUID, VELIT_WRITE_CHAR_UUID);
      if (write_chr == nullptr) {
        ESP_LOGE(TAG, "Could not find write characteristic (FFE2)");
        break;
      }
      this->write_handle_ = write_chr->handle;
      ESP_LOGI(TAG, "Found write characteristic FFE2 at handle 0x%04X", this->write_handle_);

      auto *notify_chr = this->parent_->get_characteristic(VELIT_SERVICE_UUID, VELIT_NOTIFY_CHAR_UUID);
      if (notify_chr == nullptr) {
        ESP_LOGE(TAG, "Could not find notify characteristic (FFE1)");
        break;
      }
      this->notify_handle_ = notify_chr->handle;
      ESP_LOGI(TAG, "Found notify characteristic FFE1 at handle 0x%04X", this->notify_handle_);

      this->handles_discovered_ = true;

      auto status = esp_ble_gattc_register_for_notify(
          this->parent_->get_gattc_if(),
          this->parent_->get_remote_bda(),
          this->notify_handle_);
      if (status != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register for notifications: %d", status);
      }
      break;
    }

    case ESP_GATTC_REG_FOR_NOTIFY_EVT: {
      if (param->reg_for_notify.status == ESP_GATT_OK) {
        this->notifications_enabled_ = true;
        ESP_LOGI(TAG, "Notifications enabled");

        // Send clock sync on first connection
        if (!this->clock_synced_) {
          this->send_clock_sync_();
          this->clock_synced_ = true;
        }

        // Kick off first poll
        this->poll_phase_ = 0;
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

// ---- Frame building ----

void VelitHeater::send_command_(uint8_t func, const uint8_t *data, uint8_t data_len) {
  if (!this->handles_discovered_) {
    ESP_LOGW(TAG, "Cannot send — not connected");
    return;
  }

  // Frame: 55 LEN [master 4B] [slave 4B] FUNC [data N] CHK
  uint8_t payload_len = 4 + 4 + 1 + data_len + 1;  // master+slave+func+data+chk
  uint8_t frame_len = 2 + payload_len;  // header + len byte + payload
  std::vector<uint8_t> frame(frame_len);

  frame[0] = CMD_HEADER;
  frame[1] = payload_len;

  // Master address
  memcpy(&frame[2], MASTER_ADDR, 4);
  // Slave address
  memcpy(&frame[6], SLAVE_ADDR, 4);

  frame[10] = func;

  if (data_len > 0 && data != nullptr) {
    memcpy(&frame[11], data, data_len);
  }

  // Checksum: sum of all bytes before checksum, & 0xFF
  uint8_t checksum = 0;
  for (int i = 0; i < frame_len - 1; i++) {
    checksum += frame[i];
  }
  frame[frame_len - 1] = checksum;

  ESP_LOGD(TAG, "TX: func=0x%02X data_len=%d chk=0x%02X", func, data_len, checksum);

  auto status = esp_ble_gattc_write_char(
      this->parent_->get_gattc_if(),
      this->parent_->get_conn_id(),
      this->write_handle_,
      frame_len,
      frame.data(),
      ESP_GATT_WRITE_TYPE_RSP,
      ESP_GATT_AUTH_REQ_NONE);

  if (status != ESP_OK) {
    ESP_LOGE(TAG, "BLE write failed: %d", status);
  }
}

void VelitHeater::send_command_(uint8_t func, uint8_t value) {
  send_command_(func, &value, 1);
}

void VelitHeater::send_clock_sync_() {
  time_t now_ts = time(nullptr);
  struct tm *t = localtime(&now_ts);

  if (t == nullptr || t->tm_year < 100) {
    ESP_LOGW(TAG, "Clock not available, skipping sync");
    return;
  }

  uint8_t data[7];
  data[0] = t->tm_hour;
  data[1] = t->tm_min;
  data[2] = t->tm_sec;
  data[3] = t->tm_year - 100;  // Year since 2000
  data[4] = t->tm_mon + 1;     // Month 1-12
  data[5] = t->tm_mday;
  data[6] = t->tm_wday;        // Day of week

  ESP_LOGI(TAG, "Clock sync: %02d:%02d:%02d %04d-%02d-%02d",
           data[0], data[1], data[2], data[3] + 2000, data[4], data[5]);

  send_command_(FUNC_CLOCK_SYNC, data, 7);
}

// ---- Response parsing ----

void VelitHeater::handle_notification_(const uint8_t *data, uint16_t length) {
  // Minimum: AA LEN [master 4B] [slave 4B] SF(2) FUNC DATA(1+) CHK(2)
  if (length < MIN_RESPONSE_LEN) {
    ESP_LOGD(TAG, "Short notification (%d bytes), ignoring", length);
    return;
  }

  if (data[0] != RSP_HEADER) {
    ESP_LOGD(TAG, "Not a heater response (header 0x%02X)", data[0]);
    return;
  }

  // Verify manufacturer code "SF" at positions 10-11
  if (data[10] != MANUF_CODE_S || data[11] != MANUF_CODE_F) {
    ESP_LOGW(TAG, "Invalid manufacturer code: 0x%02X 0x%02X", data[10], data[11]);
    return;
  }

  // Verify 16-bit checksum (big-endian sum of all bytes before checksum)
  uint16_t calc_sum = 0;
  for (int i = 0; i < length - 2; i++) {
    calc_sum += data[i];
  }
  uint16_t recv_sum = (data[length - 2] << 8) | data[length - 1];
  if (calc_sum != recv_sum) {
    ESP_LOGW(TAG, "Checksum mismatch: calc=0x%04X recv=0x%04X", calc_sum, recv_sum);
    return;
  }

  uint8_t func = data[12];
  const uint8_t *payload = &data[13];
  uint8_t payload_len = length - 13 - 2;  // Subtract header/prefix and 2-byte checksum

  ESP_LOGD(TAG, "RX: func=0x%02X payload_len=%d", func, payload_len);

  switch (func) {
    case FUNC_QUERY_STATUS:
      this->parse_status_(payload, payload_len);
      break;
    case FUNC_QUERY_SENSORS:
      this->parse_sensors_(payload, payload_len);
      break;
    case FUNC_STARTUP:
      ESP_LOGI(TAG, "Startup acknowledged");
      break;
    case FUNC_SHUTDOWN:
      ESP_LOGI(TAG, "Shutdown acknowledged");
      break;
    case FUNC_SET_MODE:
      ESP_LOGI(TAG, "Mode set acknowledged: 0x%02X", payload_len > 0 ? payload[0] : 0);
      break;
    case FUNC_SET_GEAR:
      ESP_LOGI(TAG, "Gear set acknowledged: %d", payload_len > 0 ? payload[0] : 0);
      break;
    case FUNC_SET_TEMP:
      ESP_LOGI(TAG, "Temp set acknowledged: %d", payload_len > 0 ? payload[0] : 0);
      break;
    default:
      ESP_LOGD(TAG, "Response for func 0x%02X", func);
      break;
  }
}

void VelitHeater::parse_status_(const uint8_t *data, uint8_t len) {
  // [fault][mode][gear][set_temp][machine_state][power_W][pump_freq]
  if (len < 7) {
    ESP_LOGW(TAG, "Status response too short: %d bytes", len);
    return;
  }

  bool changed = false;

  uint8_t fault = data[0];
  if (fault != this->fault_code_) {
    this->fault_code_ = fault;
    changed = true;
    const char *name = (fault < FAULT_NAME_COUNT) ? FAULT_NAMES[fault] : "Unknown";
    ESP_LOGI(TAG, "Fault: %d (%s)", fault, name);
  }

  uint8_t mode = data[1];
  if (mode != this->work_mode_) {
    this->work_mode_ = mode;
    changed = true;
    ESP_LOGI(TAG, "Mode: %s", mode == MODE_THERMOSTAT ? "Thermostat" : "Manual");
  }

  uint8_t gear = data[2];
  if (gear != this->current_gear_) {
    this->current_gear_ = gear;
    changed = true;
    ESP_LOGI(TAG, "Gear: %d", gear);
  }

  uint8_t temp = data[3];
  if (temp != this->set_temp_raw_) {
    this->set_temp_raw_ = temp;
    changed = true;
    ESP_LOGI(TAG, "Set temp: %d", temp);

    // Detect temperature unit from setpoint range
    if (temp >= 40 && temp <= 99) {
      this->fahrenheit_mode_ = true;
    } else if (temp >= 4 && temp <= 37) {
      this->fahrenheit_mode_ = false;
    }
  }

  uint8_t state = data[4];
  if (state != this->machine_state_) {
    this->machine_state_ = state;
    changed = true;
    const char *name = (state < STATE_NAME_COUNT) ? STATE_NAMES[state] : "Unknown";
    ESP_LOGI(TAG, "State: %d (%s)", state, name);
  }

  this->power_watts_ = data[5];
  this->pump_freq_raw_ = data[6];

  this->state_received_ = true;

  if (changed) {
    this->publish_state_();
  }
}

void VelitHeater::parse_sensors_(const uint8_t *data, uint8_t len) {
  // [fault][voltage 2B][rpm 2B][inlet 2B][casing 2B][outlet 2B][altitude 2B]
  if (len < 13) {
    ESP_LOGW(TAG, "Sensor response too short: %d bytes", len);
    return;
  }

  // Fault code (same as status)
  uint8_t fault = data[0];
  if (fault != this->fault_code_) {
    this->fault_code_ = fault;
  }

  // Voltage: ÷10 for Volts
  uint16_t volt_raw = (data[1] << 8) | data[2];
  this->voltage_ = volt_raw / 10.0f;

  // Fan RPM
  uint16_t rpm = (data[3] << 8) | data[4];
  this->fan_rpm_ = (rpm != SENSOR_UNAVAILABLE) ? rpm : 0;

  // Temperatures: raw - 60 = °F (in Fahrenheit mode)
  uint16_t inlet_raw = (data[5] << 8) | data[6];
  uint16_t casing_raw = (data[7] << 8) | data[8];
  uint16_t outlet_raw = (data[9] << 8) | data[10];

  if (inlet_raw != SENSOR_UNAVAILABLE) {
    this->inlet_temp_f_ = static_cast<float>(inlet_raw) - 60.0f;
  }
  if (casing_raw != SENSOR_UNAVAILABLE) {
    this->casing_temp_f_ = static_cast<float>(casing_raw) - 60.0f;
  }
  if (outlet_raw != SENSOR_UNAVAILABLE) {
    this->outlet_temp_f_ = static_cast<float>(outlet_raw) - 60.0f;
  }

  // Altitude
  uint16_t alt = (data[11] << 8) | data[12];
  if (alt != SENSOR_UNAVAILABLE) {
    this->altitude_ = alt;
  }

  this->sensors_valid_ = true;

  // Publish sensor entities
  if (this->voltage_sensor_ != nullptr)
    this->voltage_sensor_->publish_state(this->voltage_);
  if (this->fan_rpm_sensor_ != nullptr && rpm != SENSOR_UNAVAILABLE)
    this->fan_rpm_sensor_->publish_state(this->fan_rpm_);
  if (this->inlet_temp_sensor_ != nullptr && inlet_raw != SENSOR_UNAVAILABLE)
    this->inlet_temp_sensor_->publish_state(this->get_inlet_temp_c());
  if (this->outlet_temp_sensor_ != nullptr && outlet_raw != SENSOR_UNAVAILABLE) {
    float outlet_c = (this->outlet_temp_f_ - 32.0f) * 5.0f / 9.0f;
    this->outlet_temp_sensor_->publish_state(outlet_c);
  }
  if (this->altitude_sensor_ != nullptr && alt != SENSOR_UNAVAILABLE)
    this->altitude_sensor_->publish_state(this->altitude_);

  // Update climate entity current temp
  this->publish_state_();
}

float VelitHeater::get_inlet_temp_c() const {
  return (this->inlet_temp_f_ - 32.0f) * 5.0f / 9.0f;
}

void VelitHeater::publish_state_() {
  if (this->climate_ == nullptr) return;

  // Mode mapping
  if (this->machine_state_ == STATE_STANDBY && this->fault_code_ == 0 &&
      !this->is_running()) {
    this->climate_->mode = climate::CLIMATE_MODE_OFF;
  } else if (this->work_mode_ == MODE_THERMOSTAT) {
    this->climate_->mode = climate::CLIMATE_MODE_HEAT;
    this->climate_->preset = climate::CLIMATE_PRESET_HOME;  // "Thermostat"
  } else {
    this->climate_->mode = climate::CLIMATE_MODE_HEAT;
    this->climate_->preset = climate::CLIMATE_PRESET_NONE;  // "Manual"
  }

  // If in fault state, show as off
  if (this->fault_code_ != 0) {
    this->climate_->mode = climate::CLIMATE_MODE_OFF;
  }

  // Target temperature (convert to °C for HA)
  if (this->fahrenheit_mode_) {
    this->climate_->target_temperature =
        (static_cast<float>(this->set_temp_raw_) - 32.0f) * 5.0f / 9.0f;
  } else {
    this->climate_->target_temperature = static_cast<float>(this->set_temp_raw_);
  }

  // Current temperature from inlet sensor
  if (this->sensors_valid_) {
    this->climate_->current_temperature = this->get_inlet_temp_c();
  }

  // Fan mode (gear)
  switch (this->current_gear_) {
    case 1: this->climate_->set_custom_fan_mode_("1"); break;
    case 2: this->climate_->set_custom_fan_mode_("2"); break;
    case 3: this->climate_->set_custom_fan_mode_("3"); break;
    case 4: this->climate_->set_custom_fan_mode_("4"); break;
    case 5: this->climate_->set_custom_fan_mode_("5"); break;
    default: break;
  }

  this->climate_->publish_state();

  // Text sensors
  if (this->fault_sensor_ != nullptr) {
    const char *name = (this->fault_code_ < FAULT_NAME_COUNT)
        ? FAULT_NAMES[this->fault_code_] : "Unknown";
    this->fault_sensor_->publish_state(name);
  }
  if (this->state_sensor_ != nullptr) {
    const char *name = (this->machine_state_ < STATE_NAME_COUNT)
        ? STATE_NAMES[this->machine_state_] : "Unknown";
    this->state_sensor_->publish_state(name);
  }
}

// ---- Control methods ----

void VelitHeater::startup_manual() {
  ESP_LOGI(TAG, "Starting heater (manual mode)");
  this->command_queue_.push_back({FUNC_STARTUP, {0x01}});
}

void VelitHeater::shutdown() {
  ESP_LOGI(TAG, "Shutting down heater");
  this->command_queue_.push_back({FUNC_SHUTDOWN, {0x00}});
}

void VelitHeater::set_mode(uint8_t mode) {
  ESP_LOGI(TAG, "Setting mode: %s", mode == MODE_THERMOSTAT ? "Thermostat" : "Manual");
  this->command_queue_.push_back({FUNC_SET_MODE, {mode}});
}

void VelitHeater::set_gear(uint8_t gear) {
  if (gear < 1) gear = 1;
  if (gear > 5) gear = 5;
  ESP_LOGI(TAG, "Setting gear: %d", gear);
  this->command_queue_.push_back({FUNC_SET_GEAR, {gear}});
}

void VelitHeater::set_target_temp_f(uint8_t temp_f) {
  if (temp_f < HEATER_TEMP_MIN_F) temp_f = HEATER_TEMP_MIN_F;
  if (temp_f > HEATER_TEMP_MAX_F) temp_f = HEATER_TEMP_MAX_F;
  ESP_LOGI(TAG, "Setting target temp: %d°F", temp_f);
  this->command_queue_.push_back({FUNC_SET_TEMP, {temp_f}});
}

void VelitHeater::start_ventilation() {
  ESP_LOGI(TAG, "Starting ventilation");
  this->command_queue_.push_back({FUNC_START_VENT, {0x00}});
}

void VelitHeater::stop_ventilation() {
  ESP_LOGI(TAG, "Stopping ventilation");
  this->command_queue_.push_back({FUNC_STOP_VENT, {0x00}});
}


// ============================================================================
// VelitHeaterClimate — HA Climate entity
// ============================================================================

void VelitHeaterClimate::dump_config() {
  ESP_LOGCONFIG(TAG, "Velit Heater Climate Entity");
}

climate::ClimateTraits VelitHeaterClimate::traits() {
  auto traits = climate::ClimateTraits();

  traits.add_feature_flags(climate::CLIMATE_SUPPORTS_CURRENT_TEMPERATURE);
  traits.set_visual_min_temperature(HEATER_TEMP_MIN_C);
  traits.set_visual_max_temperature(HEATER_TEMP_MAX_C);
  traits.set_visual_temperature_step(1.0f);

  traits.set_supported_modes({
      climate::CLIMATE_MODE_OFF,
      climate::CLIMATE_MODE_HEAT,
      climate::CLIMATE_MODE_FAN_ONLY,
  });

  traits.set_supported_presets({
      climate::CLIMATE_PRESET_NONE,   // Manual mode
      climate::CLIMATE_PRESET_HOME,   // Thermostat mode
  });

  // Gear levels 1-5
  traits.set_supported_custom_fan_modes({"1", "2", "3", "4", "5"});

  return traits;
}

void VelitHeaterClimate::control(const climate::ClimateCall &call) {
  if (this->parent_ == nullptr) return;

  if (call.get_mode().has_value()) {
    auto mode = *call.get_mode();
    switch (mode) {
      case climate::CLIMATE_MODE_OFF:
        this->parent_->shutdown();
        break;
      case climate::CLIMATE_MODE_HEAT:
        this->parent_->startup_manual();
        break;
      case climate::CLIMATE_MODE_FAN_ONLY:
        this->parent_->start_ventilation();
        break;
      default:
        break;
    }
  }

  if (call.get_preset().has_value()) {
    auto preset = *call.get_preset();
    switch (preset) {
      case climate::CLIMATE_PRESET_HOME:
        this->parent_->set_mode(MODE_THERMOSTAT);
        break;
      case climate::CLIMATE_PRESET_NONE:
        this->parent_->set_mode(MODE_MANUAL);
        break;
      default:
        break;
    }
  }

  if (call.get_target_temperature().has_value()) {
    float temp_c = *call.get_target_temperature();
    uint8_t temp_f = static_cast<uint8_t>(roundf((temp_c * 9.0f / 5.0f) + 32.0f));
    this->parent_->set_target_temp_f(temp_f);
  }

  if (call.has_custom_fan_mode()) {
    auto fan_mode = call.get_custom_fan_mode();
    uint8_t gear = 0;
    if (fan_mode == "1") gear = 1;
    else if (fan_mode == "2") gear = 2;
    else if (fan_mode == "3") gear = 3;
    else if (fan_mode == "4") gear = 4;
    else if (fan_mode == "5") gear = 5;
    if (gear > 0) {
      this->parent_->set_gear(gear);
    }
  }
}

}  // namespace velit_heater
}  // namespace esphome
