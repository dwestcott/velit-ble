#include "velit_heater.h"
#include "esphome/core/helpers.h"
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
  ESP_LOGCONFIG(TAG, "  Idle poll interval: %u ms", (unsigned) this->idle_interval_ms_);
  if (!this->firmware_version_.empty())
    ESP_LOGCONFIG(TAG, "  Heater firmware: %s", this->firmware_version_.c_str());
}

void VelitHeater::update() {
  // Called by PollingComponent every update_interval — kicks off a poll cycle,
  // except in settled states, which only poll every idle_interval. Overtemp
  // Standby is the thermostat idling at setpoint (can last 40+ min); polling
  // it every 5 s coincided with missed replies upstream (velit-hass 1bdae6e).
  if (!this->is_connected())
    return;
  uint32_t now = millis();
  bool settled = this->state_received_ &&
                 (this->machine_state_ == STATE_STANDBY ||
                  this->machine_state_ == STATE_NORMAL ||
                  this->machine_state_ == STATE_OVERTEMP);
  bool recent_command = this->last_user_command_ms_ != 0 &&
                        (now - this->last_user_command_ms_) < FAST_POLL_AFTER_COMMAND_MS;
  // 500 ms slack so scheduler jitter doesn't push a 15 s cycle to 20 s
  if (settled && !recent_command && this->last_cycle_ms_ != 0 &&
      (now - this->last_cycle_ms_) + 500 < this->idle_interval_ms_)
    return;
  this->poll_phase_ = 0;
  this->last_poll_ms_ = 0;
  this->last_cycle_ms_ = now;
}

bool VelitHeater::has_fresh_data() const {
  // Survives a brief BLE reconnect; only a minute without replies goes stale
  return this->last_valid_rx_ms_ != 0 &&
         (millis() - this->last_valid_rx_ms_) < STALE_TIMEOUT_MS;
}

float VelitHeater::get_voltage() const { return this->has_fresh_data() ? this->voltage_ : NAN; }
float VelitHeater::get_fan_rpm() const { return this->has_fresh_data() ? this->fan_rpm_ : NAN; }
float VelitHeater::get_altitude() const { return this->has_fresh_data() ? this->altitude_ : NAN; }

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

  // Housekeeping once per connection, between poll cycles: clock sync as soon
  // as time is available, then the firmware query after the first status.
  if (this->is_connected() && this->poll_phase_ >= 2 &&
      (now - this->last_command_ms_) >= COMMAND_THROTTLE_MS) {
    if (!this->clock_synced_ && (now - this->last_poll_ms_) >= POLL_INTERVAL_MS) {
      this->clock_synced_ = this->send_clock_sync_();
      this->last_command_ms_ = now;
      return;
    }
    // Right after connecting the heater can answer FFFF for the version, so
    // retry every 30 s (max 5 tries per connection) until it decodes.
    bool retry_due = this->firmware_version_.empty() && this->firmware_tries_ < 5 &&
                     (now - this->firmware_query_ms_) >= 30000;
    if (this->state_received_ && (!this->firmware_requested_ || retry_due)) {
      ESP_LOGD(TAG, "Querying heater firmware version (0x6A)");
      this->send_command_(FUNC_FIRMWARE, (uint8_t) 0x01);
      this->firmware_requested_ = true;
      this->firmware_tries_++;
      this->firmware_query_ms_ = now;
      this->last_command_ms_ = now;
      return;
    }
  }

  // Stale data: republish so the climate entity drops its current temperature
  if (this->last_valid_rx_ms_ != 0 && !this->has_fresh_data() && this->climate_ != nullptr &&
      !std::isnan(this->climate_->current_temperature)) {
    ESP_LOGW(TAG, "No valid reply for %u s, marking data stale", (unsigned) (STALE_TIMEOUT_MS / 1000));
    this->publish_state_();
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
      if (this->firmware_version_.empty()) {
        this->firmware_requested_ = false;
        this->firmware_tries_ = 0;
      }
      this->poll_phase_ = 2;
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

        // Kick off first poll; loop() sends the clock sync once time is valid
        this->poll_phase_ = 0;
        this->last_poll_ms_ = 0;
        this->last_cycle_ms_ = millis();
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

  // Frame: 55 LEN [master 4B] [slave 4B] FUNC [data N] CHK(2B)
  // Same 2-byte big-endian checksum as responses (velit-hass heater_client.py).
  // Until v0.5.0 this sent a 1-byte checksum with data padded to [value, 0x00];
  // the pad sat where the checksum high byte belongs, so single-value frames
  // were byte-identical (sum < 256) but the 7-byte clock sync was malformed.
  uint8_t payload_len = 4 + 4 + 1 + data_len + 2;  // master+slave+func+data+chk
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

  // Checksum: sum of all bytes before it, & 0xFFFF, high byte first
  uint16_t checksum = 0;
  for (int i = 0; i < frame_len - 2; i++) {
    checksum += frame[i];
  }
  frame[frame_len - 2] = checksum >> 8;
  frame[frame_len - 1] = checksum & 0xFF;

  ESP_LOGD(TAG, "TX: func=0x%02X data_len=%d chk=0x%04X", func, data_len, checksum);

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

void VelitHeater::queue_command_(uint8_t func, uint8_t value) {
  this->command_queue_.push_back({func, {value}});
  this->last_user_command_ms_ = millis();
}

bool VelitHeater::send_clock_sync_() {
  time_t now_ts = time(nullptr);
  struct tm *t = localtime(&now_ts);

  if (t == nullptr || t->tm_year < 100) {
    return false;  // no time from HA yet; loop() retries
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
  return true;
}

// ---- Response parsing ----

void VelitHeater::handle_notification_(const uint8_t *data, uint16_t length) {
  // Minimum: AA LEN [master 4B] [slave 4B] SF(2) FUNC DATA(1+) CHK(2)
  if (length < MIN_RESPONSE_LEN) {
    ESP_LOGD(TAG, "Short notification: %s", format_hex_pretty(data, length).c_str());
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

  // Status/sensor payloads in hex so unconfirmed fields (fan RPM, power,
  // pump frequency) can be checked against what the heater is doing
  if (func == FUNC_QUERY_STATUS || func == FUNC_QUERY_SENSORS) {
    ESP_LOGD(TAG, "RX: func=0x%02X %s", func, format_hex_pretty(payload, payload_len).c_str());
  } else {
    ESP_LOGD(TAG, "RX: func=0x%02X payload_len=%d", func, payload_len);
  }
  ESP_LOGV(TAG, "RX raw: %s", format_hex_pretty(data, length).c_str());

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
    case FUNC_FIRMWARE:
      // Full reply logged so other firmware (e.g. 6.x) layouts can be checked
      ESP_LOGI(TAG, "Firmware reply: %s", format_hex_pretty(payload, payload_len).c_str());
      this->firmware_reply_ = format_hex(payload, payload_len);
      // Upstream reads bytes 14-15 (0x0139 = "3.13", one heater). Other heaters
      // send FFFF there, so only decode a real value; the raw reply is kept.
      if (payload_len >= 16 && ((payload[14] << 8) | payload[15]) != SENSOR_UNAVAILABLE) {
        uint16_t val = (payload[14] << 8) | payload[15];
        char buf[12];
        snprintf(buf, sizeof(buf), "%u.%02u", val / 100, val % 100);
        this->firmware_version_ = buf;
        ESP_LOGI(TAG, "Heater firmware version: %s", buf);
      } else {
        ESP_LOGW(TAG, "Firmware reply has no version at bytes 14-15 (try %u)", this->firmware_tries_);
      }
      break;
    default:
      // Unrecognised function: log the payload for protocol discovery
      ESP_LOGD(TAG, "Response for func 0x%02X: %s", func,
               format_hex_pretty(payload, payload_len).c_str());
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
    const char *name = (fault < FAULT_NAME_COUNT) ? FAULT_NAMES[fault] : "unmapped";
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
    const char *name = (state < STATE_NAME_COUNT) ? STATE_NAMES[state] : "unmapped";
    ESP_LOGI(TAG, "State: %d (%s)", state, name);
  }

  this->power_watts_ = data[5];
  this->pump_freq_raw_ = data[6];

  this->state_received_ = true;
  this->last_valid_rx_ms_ = millis();

  if (changed) {
    this->publish_state_();
  }
}

void VelitHeater::parse_sensors_(const uint8_t *data, uint8_t len) {
  // [fault][voltage 2B][rpm 2B][inlet 2B][casing 2B][outlet 2B][altitude 2B]
  // 0xFFFF in any 16-bit field = sensor unavailable; the last value is held.
  if (len < 13) {
    ESP_LOGW(TAG, "Sensor response too short: %d bytes", len);
    return;
  }

  // Fault code (same as status)
  this->fault_code_ = data[0];

  // Voltage: ÷10 for Volts
  uint16_t volt_raw = (data[1] << 8) | data[2];
  if (volt_raw != SENSOR_UNAVAILABLE)
    this->voltage_ = volt_raw / 10.0f;

  // Fan RPM
  uint16_t rpm = (data[3] << 8) | data[4];
  if (rpm != SENSOR_UNAVAILABLE)
    this->fan_rpm_ = rpm;

  // Inlet (room) temperature; casing/outlet aren't exposed
  uint16_t inlet_raw = (data[5] << 8) | data[6];
  if (inlet_raw != SENSOR_UNAVAILABLE)
    this->inlet_temp_c_ = this->raw_temp_to_c_(inlet_raw);

  // Altitude in the heater's unit (ft in °F mode, m in °C mode)
  uint16_t alt = (data[11] << 8) | data[12];
  uint16_t max_alt = this->fahrenheit_mode_ ? MAX_ALTITUDE_FT : MAX_ALTITUDE_M;
  if (alt != SENSOR_UNAVAILABLE && alt <= max_alt) {
    this->altitude_ = alt;
  } else if (alt != SENSOR_UNAVAILABLE) {
    ESP_LOGD(TAG, "Ignoring implausible altitude %u", alt);
  }

  this->sensors_valid_ = true;
  this->last_valid_rx_ms_ = millis();

  // Climate entity current temp (publish_state_ skips it if nothing changed)
  this->publish_state_();
}

float VelitHeater::raw_temp_to_c_(uint16_t raw) const {
  // Protocol offsets: raw - 60 = °F, raw - 50 = °C
  if (this->fahrenheit_mode_)
    return (static_cast<float>(raw) - 60.0f - 32.0f) * 5.0f / 9.0f;
  return static_cast<float>(raw) - 50.0f;
}

float VelitHeater::get_inlet_temp_c() const {
  return this->has_fresh_data() ? this->inlet_temp_c_ : NAN;
}

void VelitHeater::publish_state_() {
  if (this->climate_ == nullptr) return;

  bool fresh = this->has_fresh_data();
  bool fault = this->fault_code_ != 0;
  uint8_t st = this->machine_state_;

  // Mode: OFF when idle, cooling down after shutdown, or faulted
  climate::ClimateMode mode =
      (fault || st == STATE_STANDBY || st == STATE_COOLING_DOWN)
          ? climate::CLIMATE_MODE_OFF : climate::CLIMATE_MODE_HEAT;

  // Action: what the heater is doing right now
  climate::ClimateAction action;
  if (fault || st == STATE_STANDBY) {
    action = climate::CLIMATE_ACTION_OFF;
  } else if (st == STATE_NORMAL || st == STATE_CLEANING) {
    action = climate::CLIMATE_ACTION_HEATING;
  } else if (st == STATE_COOLING_DOWN) {
    action = climate::CLIMATE_ACTION_FAN;
  } else {
    action = climate::CLIMATE_ACTION_IDLE;  // Overtemp Standby, Clean Complete
  }

  climate::ClimatePreset preset = (this->work_mode_ == MODE_THERMOSTAT)
      ? climate::CLIMATE_PRESET_HOME    // "Thermostat"
      : climate::CLIMATE_PRESET_NONE;   // "Manual"

  float target = this->fahrenheit_mode_
      ? (static_cast<float>(this->set_temp_raw_) - 32.0f) * 5.0f / 9.0f
      : static_cast<float>(this->set_temp_raw_);
  float current = (fresh && this->sensors_valid_) ? this->inlet_temp_c_ : NAN;

  // Skip the publish if nothing HA shows has changed (was every poll)
  auto same = [](float a, float b) {
    return (std::isnan(a) && std::isnan(b)) || a == b;
  };
  if (this->published_once_ && this->climate_->mode == mode &&
      this->climate_->action == action && this->climate_->preset == preset &&
      same(this->climate_->target_temperature, target) &&
      same(this->climate_->current_temperature, current) &&
      this->published_gear_ == this->current_gear_)
    return;

  this->climate_->mode = mode;
  this->climate_->action = action;
  this->climate_->preset = preset;
  this->climate_->target_temperature = target;
  this->climate_->current_temperature = current;

  // Fan mode (gear)
  switch (this->current_gear_) {
    case 1: this->climate_->set_custom_fan_mode_("1"); break;
    case 2: this->climate_->set_custom_fan_mode_("2"); break;
    case 3: this->climate_->set_custom_fan_mode_("3"); break;
    case 4: this->climate_->set_custom_fan_mode_("4"); break;
    case 5: this->climate_->set_custom_fan_mode_("5"); break;
    default: break;
  }
  this->published_gear_ = this->current_gear_;
  this->published_once_ = true;

  this->climate_->publish_state();
}

// ---- Control methods ----

void VelitHeater::startup() {
  // The startup data byte is the work mode; sending Manual (0x01) while the
  // heater is in Thermostat mode switched it to Manual (velit-hass 4c30cd6).
  uint8_t mode = (this->work_mode_ == MODE_THERMOSTAT) ? MODE_THERMOSTAT : MODE_MANUAL;
  ESP_LOGI(TAG, "Starting heater (%s mode)", mode == MODE_THERMOSTAT ? "thermostat" : "manual");
  this->queue_command_(FUNC_STARTUP, mode);
}

void VelitHeater::startup_manual() { this->startup(); }

void VelitHeater::shutdown() {
  ESP_LOGI(TAG, "Shutting down heater");
  this->queue_command_(FUNC_SHUTDOWN, 0x00);
}

void VelitHeater::set_mode(uint8_t mode) {
  ESP_LOGI(TAG, "Setting mode: %s", mode == MODE_THERMOSTAT ? "Thermostat" : "Manual");
  this->queue_command_(FUNC_SET_MODE, mode);
}

void VelitHeater::set_gear(uint8_t gear) {
  if (gear < 1) gear = 1;
  if (gear > 5) gear = 5;
  ESP_LOGI(TAG, "Setting gear: %d", gear);
  this->queue_command_(FUNC_SET_GEAR, gear);
}

void VelitHeater::set_target_temp_f(uint8_t temp_f) {
  if (temp_f < HEATER_TEMP_MIN_F) temp_f = HEATER_TEMP_MIN_F;
  if (temp_f > HEATER_TEMP_MAX_F) temp_f = HEATER_TEMP_MAX_F;
  ESP_LOGI(TAG, "Setting target temp: %d°F", temp_f);
  this->queue_command_(FUNC_SET_TEMP, temp_f);
}

void VelitHeater::set_target_temp_c(float temp_c) {
  if (this->fahrenheit_mode_) {
    this->set_target_temp_f(static_cast<uint8_t>(roundf((temp_c * 9.0f / 5.0f) + 32.0f)));
    return;
  }
  float c = roundf(temp_c);
  if (c < HEATER_TEMP_MIN_C) c = HEATER_TEMP_MIN_C;
  if (c > HEATER_TEMP_MAX_C) c = HEATER_TEMP_MAX_C;
  ESP_LOGI(TAG, "Setting target temp: %d°C", (int) c);
  this->queue_command_(FUNC_SET_TEMP, static_cast<uint8_t>(c));
}

void VelitHeater::start_ventilation() {
  ESP_LOGI(TAG, "Starting ventilation");
  this->queue_command_(FUNC_START_VENT, 0x00);
}

void VelitHeater::stop_ventilation() {
  ESP_LOGI(TAG, "Stopping ventilation");
  this->queue_command_(FUNC_STOP_VENT, 0x00);
}

void VelitHeater::start_fuel_prime() {
  ESP_LOGI(TAG, "Starting fuel prime (30s auto-stop)");
  this->queue_command_(FUNC_START_PRIME, 0x00);
}

void VelitHeater::stop_fuel_prime() {
  ESP_LOGI(TAG, "Stopping fuel prime");
  this->queue_command_(FUNC_STOP_PRIME, 0x00);
}

void VelitHeater::start_cleaning() {
  ESP_LOGI(TAG, "Starting cleaning cycle (cannot be manually stopped)");
  this->queue_command_(FUNC_CLEANING, 0x00);
}


// ============================================================================
// VelitHeaterClimate — HA Climate entity
// ============================================================================

void VelitHeaterClimate::dump_config() {
  ESP_LOGCONFIG(TAG, "Velit Heater Climate Entity");
}

climate::ClimateTraits VelitHeaterClimate::traits() {
  auto traits = climate::ClimateTraits();

  traits.add_feature_flags(climate::CLIMATE_SUPPORTS_CURRENT_TEMPERATURE |
                           climate::CLIMATE_SUPPORTS_ACTION);
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

  // Gear levels 1-5 are registered on the Climate entity in setup() (per
  // ESPHome 2026.11+ API) — ClimateTraits will pick them up automatically.

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
        this->parent_->startup();
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
    this->parent_->set_target_temp_c(*call.get_target_temperature());
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
