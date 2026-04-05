#include "zehnder.h"
#include "esphome/core/log.h"
#include "esphome/core/application.h"

namespace esphome {
namespace zehnder {

#define MAX_TRANSMIT_TIME 2000

static const char *const TAG = "zehnder";

typedef struct __attribute__((packed)) {
  uint32_t networkId;
} RfPayloadNetworkJoinOpen;

typedef struct __attribute__((packed)) {
  uint32_t networkId;
} RfPayloadNetworkJoinRequest;

typedef struct __attribute__((packed)) {
  uint32_t networkId;
} RfPayloadNetworkJoinAck;

typedef struct __attribute__((packed)) {
  uint8_t speed;
  uint8_t voltage;
  uint8_t timer;
} RfPayloadFanSettings;

typedef struct __attribute__((packed)) {
  uint8_t speed;
} RfPayloadFanSetSpeed;

typedef struct __attribute__((packed)) {
  uint8_t speed;
  uint8_t timer;
} RfPayloadFanSetTimer;

typedef struct __attribute__((packed)) {
  uint8_t rx_type;          // 0x00 RX Type
  uint8_t rx_id;            // 0x01 RX ID
  uint8_t tx_type;          // 0x02 TX Type
  uint8_t tx_id;            // 0x03 TX ID
  uint8_t ttl;              // 0x04 Time-To-Live
  uint8_t command;          // 0x05 Frame type
  uint8_t parameter_count;  // 0x06 Number of parameters

  union {
    uint8_t parameters[9];                           // 0x07 - 0x0F Depends on command
    RfPayloadFanSetSpeed setSpeed;                   // Command 0x02
    RfPayloadFanSetTimer setTimer;                   // Command 0x03
    RfPayloadNetworkJoinRequest networkJoinRequest;  // Command 0x04
    RfPayloadNetworkJoinOpen networkJoinOpen;        // Command 0x06
    RfPayloadFanSettings fanSettings;                // Command 0x07
    RfPayloadNetworkJoinAck networkJoinAck;          // Command 0x0C
  } payload;
} RfFrame;

ZehnderRF::ZehnderRF(void) {
  ESP_LOGE("zehnder", "!!! CONSTRUCTOR CALLED !!!");
}

void ZehnderRF::set_rf(nrf905::nRF905 *const pRf) {
  ESP_LOGE(TAG, "!!! set_rf() CALLED !!!");
  rf_ = pRf;
}

fan::FanTraits ZehnderRF::get_traits() { return fan::FanTraits(false, true, false, this->speed_count_); }

void ZehnderRF::control(const fan::FanCall &call) {
  ESP_LOGI(TAG, "=== FAN CONTROL CALLED ===");

  if (call.get_state().has_value()) {
    this->state = *call.get_state();
    ESP_LOGI(TAG, "Control state: %s", this->state ? "ON" : "OFF");
  }

  if (call.get_speed().has_value()) {
    this->speed = *call.get_speed();
    ESP_LOGI(TAG, "Control speed: %d (0x%02X)", this->speed, this->speed);
  }

  ESP_LOGI(TAG, "Speed count configured: %d", this->speed_count_);
  ESP_LOGI(TAG, "Final speed to send: %d (state=%s)",
           this->state ? this->speed : 0, this->state ? "ON" : "OFF");

  switch (this->state_) {
    case StateIdle:
      // Map HA speed to Zehnder preset (DIRECT 1:1):
      // OFF → Preset 0 (real OFF, 0 volt)
      // Speed 1 (25%) → Preset 1 (Low)
      // Speed 2 (50%) → Preset 2 (Medium)
      // Speed 3 (75%) → Preset 3 (High)
      // Speed 4 (100%) → Preset 4 (Max)
      uint8_t zehnder_preset;
      if (!this->state) {
        zehnder_preset = 0;  // OFF → Preset 0
      } else {
        zehnder_preset = this->speed;  // Direct 1:1: HA speed 1-4 → preset 1-4
      }

      ESP_LOGI(TAG, "Sending to fan: HA speed %d (state=%s) → Zehnder preset %d",
               this->speed, this->state ? "ON" : "OFF", zehnder_preset);
      this->setSpeed(zehnder_preset, 0);

      this->lastFanQuery_ = millis();  // Update time
      break;

    default:
      ESP_LOGW(TAG, "Fan control called but not in Idle state (state: 0x%02X)", this->state_);
      break;
  }

  this->publish_state();
  ESP_LOGI(TAG, "=== FAN CONTROL COMPLETE ===");
}

void ZehnderRF::setup() {
  ESP_LOGE(TAG, "========================================");
  ESP_LOGE(TAG, "!!! ZEHNDER SETUP() CALLED !!!");
  ESP_LOGE(TAG, "Setup priority: %.1f (nRF905 is at 600.0)", this->get_setup_priority());
  ESP_LOGE(TAG, "nRF905::setup() already completed (higher priority runs first)");
  ESP_LOGE(TAG, "========================================");

  // Clear config
  memset(&this->config_, 0, sizeof(Config));

  uint32_t hash = fnv1_hash("zehnderrf");
  this->pref_ = global_preferences->make_preference<Config>(hash, true);
  this->config_loaded_ = false;
  if (this->pref_.load(&this->config_)) {
    ESP_LOGW(TAG, "Config loaded from preferences:");
    ESP_LOGW(TAG, "  Network ID: 0x%08X", this->config_.fan_networkId);
    ESP_LOGW(TAG, "  My Type: 0x%02X, My ID: 0x%02X", this->config_.fan_my_device_type, this->config_.fan_my_device_id);
    ESP_LOGW(TAG, "  Main Type: 0x%02X, Main ID: 0x%02X", this->config_.fan_main_unit_type, this->config_.fan_main_unit_id);

    // Check if config looks valid (paired)
    if (this->config_.fan_networkId == 0xFE75FD9B &&
        this->config_.fan_my_device_type == FAN_TYPE_RF_REMOTE &&
        this->config_.fan_my_device_id != 0 &&
        this->config_.fan_main_unit_id != 0) {
      this->config_loaded_ = true;
      ESP_LOGW(TAG, "✓ Valid pairing configuration found - will skip auto-pairing");

      // CRITICAL FIX: Always force target to MAIN_UNIT (0x01) regardless of saved value
      // Old configs may have MAIN_CONTROL (0x0E) which doesn't work for commands
      if (this->config_.fan_main_unit_type != FAN_TYPE_MAIN_UNIT) {
        ESP_LOGW(TAG, "  Fixing target type from 0x%02X → 0x01 (MAIN_UNIT)", this->config_.fan_main_unit_type);
        this->config_.fan_main_unit_type = FAN_TYPE_MAIN_UNIT;
        this->pref_.save(&this->config_);  // Save corrected config
      }
      ESP_LOGW(TAG, "  Using target type: 0x%02X", this->config_.fan_main_unit_type);
    } else {
      ESP_LOGW(TAG, "✗ Pairing config invalid or incomplete - will auto-pair");
    }
  }

  ESP_LOGE(TAG, "Checking nRF905 component...");
  if (this->rf_ == nullptr) {
    ESP_LOGE(TAG, "ERROR: nRF905 component is NULL! Cannot continue setup.");
    return;
  }
  ESP_LOGE(TAG, "nRF905 component OK");

  this->speed_count_ = 4;  // 4 speeds (HA 1-4 → presets 1-4, OFF → preset 0)

  // === Register TX callback ===
  // Note: nRF905::setup() runs BEFORE this (priority 600 vs 599)
  // So hardware is already initialized by ESPHome
  this->rf_->setOnTxReady([this](void) {
    ESP_LOGD(TAG, "Tx Ready");
    if (this->rfState_ == RfStateTxBusy) {
      if (this->retries_ >= 0) {
        this->msgSendTime_ = millis();
        this->rfState_ = RfStateRxWait;
      } else {
        this->rfState_ = RfStateIdle;
      }
    }
  });
  ESP_LOGE(TAG, ">>> TX Callback registered");

  // === Configure RF parameters BEFORE device config (exact manual_init order!) ===
  ESP_LOGE(TAG, ">>> Configuring nRF905 for BOXSTREAM network...");
  nrf905::Config rfConfig = this->rf_->getConfig();
  rfConfig.band = true;
  rfConfig.channel = 117;  // 868.2 MHz for BOXSTREAM/BUVA
  rfConfig.crc_enable = true;
  rfConfig.crc_bits = 16;
  rfConfig.tx_power = 10;
  rfConfig.rx_power = nrf905::PowerNormal;
  rfConfig.rx_address = 0xFE75FD9B;  // BOXSTREAM network
  rfConfig.rx_address_width = 4;
  rfConfig.rx_payload_width = 16;
  rfConfig.tx_address_width = 4;
  rfConfig.tx_payload_width = 16;
  rfConfig.xtal_frequency = 16000000;
  rfConfig.clkOutFrequency = nrf905::ClkOut500000;
  rfConfig.clkOutEnable = false;

  this->rf_->updateConfig(&rfConfig);
  this->rf_->writeTxAddress(0xFE75FD9B);
  ESP_LOGE(TAG, ">>> nRF905 fully configured");

  // === NOW configure device identity AFTER RF config (manual_init order!) ===
  ESP_LOGE(TAG, ">>> Configuring device identity...");

  this->speed_count_ = 4;  // 4 speeds (HA 1-4 → presets 1-4, OFF → preset 0)

  // If no valid config was loaded, generate fresh defaults with a random device ID
  if (!this->config_loaded_) {
    ESP_LOGE(TAG, ">>> No valid config found - generating fresh config with random device ID");
    this->config_.fan_networkId = 0xFE75FD9B;
    this->config_.fan_my_device_type = FAN_TYPE_RF_REMOTE;  // 0x0F (like bathroom remote)
    this->config_.fan_my_device_id = this->createDeviceID();  // Random ID each time (not 0x00/0xFF)
    this->config_.fan_main_unit_type = FAN_TYPE_MAIN_UNIT;  // 0x01 - Commands go to MAIN_UNIT!
    this->config_.fan_main_unit_id = 0x39;  // Main unit ID
  }

  ESP_LOGE(TAG, ">>> Device: RF_REMOTE (0x0F) ID=0x%02X → Target: MAIN_UNIT (0x01) ID=0x%02X",
           this->config_.fan_my_device_id, this->config_.fan_main_unit_id);

  // === Register RX callback (TX callback registered earlier by nRF905 or stays from previous setup) ===
  this->rf_->setOnRxComplete([this](const uint8_t *const pData, const uint8_t dataLength) {
    ESP_LOGE(TAG, "!!! RX CALLBACK - FRAME RECEIVED !!!");
    this->rfHandleReceived(pData, dataLength);
  });
  ESP_LOGE(TAG, ">>> RX Callback registered (TX callback inherited)");

  // Enable promiscuous mode (like manual_init)
  this->rf_->setPromiscuousMode(true);
  ESP_LOGE(TAG, ">>> Promiscuous mode enabled");

  // Start in receive mode (like manual_init - no delay, no publish before this)
  this->rf_->setMode(nrf905::Receive);
  ESP_LOGE(TAG, ">>> nRF905 set to RECEIVE mode");

  // Restore fan state from preferences (ESPHome restore_mode support)
  auto restore = this->restore_state_();
  if (restore.has_value()) {
    ESP_LOGE(TAG, ">>> restore_state_() returned value!");
    ESP_LOGE(TAG, ">>> Before apply: state=%s, speed=%d", this->state ? "ON" : "OFF", this->speed);
    restore->apply(*this);
    ESP_LOGE(TAG, ">>> After apply: state=%s, speed=%d", this->state ? "ON" : "OFF", this->speed);

    // If state is ON but speed is 0, default to speed 1 (Low)
    if (this->state && this->speed == 0) {
      ESP_LOGW(TAG, ">>> State ON but speed 0, defaulting to speed 1");
      this->speed = 1;
    }
  } else {
    ESP_LOGE(TAG, ">>> restore_state_() returned NO value (no saved state)");
    // No saved state, default to OFF
    this->state = false;
    this->speed = 0;
  }
  ESP_LOGE(TAG, ">>> Final state: %s, speed: %d", this->state ? "ON" : "OFF", this->speed);
  this->publish_state();

  // Decide whether to pair or go straight to Idle
  if (this->config_loaded_) {
    // Already paired - go straight to Idle for immediate fan control
    this->state_ = StateIdle;
    ESP_LOGE(TAG, "========================================");
    ESP_LOGE(TAG, "ZEHNDER FAN READY - Already paired!");
    ESP_LOGE(TAG, "Fan control enabled immediately");
    ESP_LOGE(TAG, "Fan state: %s, Speed: %d", this->state ? "ON" : "OFF", this->speed);
    ESP_LOGE(TAG, "Current target: Type=0x%02X, ID=0x%02X",
             this->config_.fan_main_unit_type, this->config_.fan_main_unit_id);
    ESP_LOGE(TAG, "========================================");
  } else {
    // Not paired yet - start pairing sequence in loop()
    this->state_ = StateStartup;
    ESP_LOGE(TAG, "========================================");
    ESP_LOGE(TAG, "ZEHNDER FAN SETUP COMPLETE");
    ESP_LOGE(TAG, "State set to: StateStartup (0x%02X)", this->state_);
    ESP_LOGE(TAG, "Will automatically pair with fan after 5 seconds...");
    ESP_LOGE(TAG, "========================================");
  }

  this->initialized_ = true;
}

void ZehnderRF::manual_init() {
  ESP_LOGE(TAG, "========================================");
  ESP_LOGE(TAG, "!!! MANUAL_INIT() CALLED VIA BUTTON !!!");
  ESP_LOGE(TAG, "========================================");

  if (this->rf_ == nullptr) {
    ESP_LOGE(TAG, "ERROR: nRF905 component is NULL!");
    return;
  }
  ESP_LOGE(TAG, "nRF905 component OK at %p", this->rf_);

  // CRITICAL: Manually call nRF905 setup() - ESPHome never calls it!
  ESP_LOGE(TAG, "Calling nRF905 setup() manually...");
  this->rf_->setup();
  ESP_LOGE(TAG, "nRF905 setup() completed");

  // Do the initialization
  nrf905::Config rfConfig;
  rfConfig = this->rf_->getConfig();

  rfConfig.band = true;
  rfConfig.channel = 117;
  rfConfig.crc_enable = true;
  rfConfig.crc_bits = 16;
  rfConfig.tx_power = 10;
  rfConfig.rx_power = nrf905::PowerNormal;
  rfConfig.rx_address = 0xFE75FD9B;
  rfConfig.rx_address_width = 4;
  rfConfig.rx_payload_width = 16;
  rfConfig.tx_address_width = 4;
  rfConfig.tx_payload_width = 16;
  rfConfig.xtal_frequency = 16000000;
  rfConfig.clkOutFrequency = nrf905::ClkOut500000;
  rfConfig.clkOutEnable = false;

  this->rf_->updateConfig(&rfConfig);
  this->rf_->writeTxAddress(0xFE75FD9B);

  this->speed_count_ = 4;  // 4 speeds (HA 1-4 → presets 1-4, OFF → preset 0)

  // Configure device identity as RF_REMOTE (type 0x0F) - same as bathroom remote
  this->config_.fan_networkId = 0xFE75FD9B;
  this->config_.fan_my_device_type = FAN_TYPE_RF_REMOTE;  // 0x0F (like bathroom remote)
  // Only generate a new random ID if none is set yet (don't overwrite a saved paired ID)
  if (this->config_.fan_my_device_id == 0x00 || this->config_.fan_my_device_id == 0xFF) {
    this->config_.fan_my_device_id = this->createDeviceID();
  }
  this->config_.fan_main_unit_type = FAN_TYPE_MAIN_UNIT;  // 0x01 - Commands go to MAIN_UNIT!
  this->config_.fan_main_unit_id = 0x39;  // Main unit ID

  ESP_LOGE(TAG, "Device configured as RF_REMOTE (0x0F) with ID 0x%02X", this->config_.fan_my_device_id);
  ESP_LOGE(TAG, "Target: MAIN_UNIT (0x01) with ID 0x%02X", this->config_.fan_main_unit_id);

  this->rf_->setOnRxComplete([this](const uint8_t *const pData, const uint8_t dataLength) {
    ESP_LOGE(TAG, "!!! RX CALLBACK - FRAME RECEIVED !!!");
    this->rfHandleReceived(pData, dataLength);
  });

  // Enable promiscuous mode to receive all broadcasts (STATUS_BROADCAST, etc.)
  this->rf_->setPromiscuousMode(true);
  ESP_LOGE(TAG, "Promiscuous mode enabled - will receive broadcasts from all devices");

  this->rf_->setMode(nrf905::Receive);

  // Set state to Idle so fan control works
  this->state_ = StateIdle;

  // Initialize fan state to OFF
  this->state = false;
  this->speed = 0;
  this->publish_state();

  ESP_LOGE(TAG, "========================================");
  ESP_LOGE(TAG, "MANUAL INIT COMPLETE - READY FOR CONTROL!");
  ESP_LOGE(TAG, "State set to Idle - fan control enabled");
  ESP_LOGE(TAG, "========================================");

  this->initialized_ = true;
}

void ZehnderRF::status_check() {
  ESP_LOGE(TAG, "========================================");
  ESP_LOGE(TAG, "STATUS CHECK:");
  ESP_LOGE(TAG, "  Initialized: %s", this->initialized_ ? "YES" : "NO");
  ESP_LOGE(TAG, "  State: 0x%02X", this->state_);
  ESP_LOGE(TAG, "  RF State: 0x%02X", this->rfState_);
  ESP_LOGE(TAG, "  nRF905 pointer: %p", this->rf_);
  ESP_LOGE(TAG, "  Config:");
  ESP_LOGE(TAG, "    Network ID: 0x%08X", this->config_.fan_networkId);
  ESP_LOGE(TAG, "    My Type: 0x%02X, My ID: 0x%02X", this->config_.fan_my_device_type, this->config_.fan_my_device_id);
  ESP_LOGE(TAG, "    Main Type: 0x%02X, Main ID: 0x%02X", this->config_.fan_main_unit_type, this->config_.fan_main_unit_id);
  ESP_LOGE(TAG, "========================================");

  // Force back to receive mode
  if (this->rf_ != nullptr && this->initialized_) {
    ESP_LOGE(TAG, "Forcing nRF905 back to RECEIVE mode...");
    this->rf_->setMode(nrf905::Receive);
    ESP_LOGE(TAG, "Mode set to RECEIVE");
  }
}

void ZehnderRF::clear_config() {
  ESP_LOGE(TAG, "========================================");
  ESP_LOGE(TAG, "CLEARING SAVED CONFIG");
  ESP_LOGE(TAG, "========================================");

  // Clear in-memory config
  memset(&this->config_, 0, sizeof(Config));

  // Clear flash
  this->pref_.save(&this->config_);

  ESP_LOGE(TAG, "Config cleared from flash");
  ESP_LOGE(TAG, "Please reboot device to trigger automatic pairing");
  ESP_LOGE(TAG, "========================================");
}

void ZehnderRF::pair_as_remote() {
  ESP_LOGE(TAG, "========================================");
  ESP_LOGE(TAG, "PAIRING SEQUENCE START");
  ESP_LOGE(TAG, "Device Type: 0x%02X (RF_REMOTE)", this->config_.fan_my_device_type);
  ESP_LOGE(TAG, "Device ID: 0x%02X", this->config_.fan_my_device_id);
  ESP_LOGE(TAG, "========================================");

  if (this->rf_ == nullptr) {
    ESP_LOGE(TAG, "ERROR: nRF905 not initialized!");
    return;
  }

  if (this->config_.fan_my_device_type == 0x00 || this->config_.fan_my_device_id == 0x00) {
    ESP_LOGE(TAG, "ERROR: Config not initialized! Call 'Manual Init' button first!");
    return;
  }

  RfFrame *const pFrame = (RfFrame *) this->_txFrame;

  // nRF905 address handling for pairing:
  // - RX address = LINK_ID: required to RECEIVE JOIN_OPEN (fan sends with TX_ADDRESS = LINK_ID)
  //   The nRF905 only raises DR when address matches; promiscuous mode does NOT override this.
  // - TX address = LINK_ID: required so fan can receive our JOIN_ACK frames.
  //   After sending JOIN_OPEN, the fan listens on LINK_ID for JOIN_ACK(NETWORK_ID) confirmation.
  //   If we send on NETWORK_ID, the fan never sees it and won't advance to accept JOIN_REQUEST.
  //   We switch TX back to NETWORK_ID before sending JOIN_REQUEST.
  nrf905::Config rfConfig = this->rf_->getConfig();
  rfConfig.rx_address = NETWORK_LINK_ID;
  this->rf_->updateConfig(&rfConfig, NULL);
  this->rf_->writeTxAddress(NETWORK_LINK_ID);  // TX = LINK_ID for JOIN_ACK frames

  memset(this->_txFrame, 0, FAN_FRAMESIZE);
  pFrame->rx_type = 0x04;  // Joining mode indicator
  pFrame->rx_id = 0x00;
  pFrame->tx_type = this->config_.fan_my_device_type;  // 0x0F (RF_REMOTE)
  pFrame->tx_id = this->config_.fan_my_device_id;
  pFrame->ttl = FAN_TTL;
  pFrame->command = FAN_NETWORK_JOIN_ACK;
  pFrame->parameter_count = sizeof(RfPayloadNetworkJoinAck);
  pFrame->payload.networkJoinAck.networkId = NETWORK_LINK_ID;  // LINK_ID in payload (protocol field)

  ESP_LOGE(TAG, "RX=LINK_ID (to receive JOIN_OPEN), TX=LINK_ID (for fan to receive our JOIN_ACK)");

  // Send JOIN_ACK and hand off to the async state machine
  // The state machine will:
  //   StateDiscoveryWaitForLinkRequest: wait for JOIN_OPEN → send JOIN_REQUEST
  //   StateDiscoveryWaitForJoinResponse: wait for FRAME_0B → send FRAME_0B ack
  //   StateDiscoveryJoinComplete: wait for QUERY_NETWORK → save config → StateIdle
  this->startTransmit(this->_txFrame, FAN_TX_RETRIES, [this]() {
    ESP_LOGW(TAG, "Pairing: JOIN_ACK timeout - fan not in pairing mode. Press 'Pair as Remote' again after enabling pairing mode on fan.");
    // Restore rx+TX to NETWORK_ID before going Idle (avoid stuck-on-LINK_ID bug)
    nrf905::Config rfCfg = this->rf_->getConfig();
    rfCfg.rx_address = this->config_.fan_networkId;
    this->rf_->updateConfig(&rfCfg, NULL);
    this->rf_->writeTxAddress(this->config_.fan_networkId);
    this->state_ = StateIdle;
  });

  this->state_ = StateDiscoveryWaitForLinkRequest;
  ESP_LOGE(TAG, "Waiting for JOIN_OPEN from fan (fan must be in pairing mode)...");
}

void ZehnderRF::dump_config(void) {
  ESP_LOGE(TAG, "!!! dump_config() CALLED !!!");
  ESP_LOGE(TAG, "========================================");
  ESP_LOGE(TAG, "SETUP STATUS CHECK:");
  ESP_LOGE(TAG, "  initialized_ flag: %s", this->initialized_ ? "TRUE" : "FALSE");
  ESP_LOGE(TAG, "  Current state_: 0x%02X", this->state_);
  ESP_LOGE(TAG, "  nRF905 pointer: %p", this->rf_);
  ESP_LOGE(TAG, "========================================");
  ESP_LOGCONFIG(TAG, "Zehnder Fan config:");
  ESP_LOGCONFIG(TAG, "  Polling interval   %u", this->interval_);
  ESP_LOGCONFIG(TAG, "  Fan networkId      0x%08X", this->config_.fan_networkId);
  ESP_LOGCONFIG(TAG, "  Fan my device type 0x%02X", this->config_.fan_my_device_type);
  ESP_LOGCONFIG(TAG, "  Fan my device id   0x%02X", this->config_.fan_my_device_id);
  ESP_LOGCONFIG(TAG, "  Fan main_unit type 0x%02X", this->config_.fan_main_unit_type);
  ESP_LOGCONFIG(TAG, "  Fan main unit id   0x%02X", this->config_.fan_main_unit_id);
  ESP_LOGE(TAG, "========================================");
}

void ZehnderRF::loop(void) {
  // Variables for old state machine code (kept for compatibility)
  uint8_t deviceId = 0;
  bool newSetting = false;
  uint8_t newSpeed = 0;
  uint8_t newTimer = 0;

  // Call nRF905 loop to process RF frames (RX/TX state machine)
  if (this->rf_ != nullptr) {
    this->rf_->loop();
  }

  // Run our own RF state machine
  this->rfHandler();

  switch (this->state_) {
    case StateStartup:
      // Wait until started up
      if (millis() > 5000) {
        // Whether config is loaded or not: go to Idle.
        // If no config: wait for user to press "Pair as Remote". No auto-pairing.
        // Auto-pairing caused an infinite TX loop (new random ID each cycle) which
        // confused the fan and prevented JOIN_OPEN from being received cleanly.
        if (this->config_loaded_) {
          ESP_LOGE(TAG, "Valid config loaded - going to Idle");
        } else {
          ESP_LOGE(TAG, "No config - going to Idle. Press 'Pair as Remote' to pair.");
        }
        this->state_ = StateIdle;
      }
      break;

    case StateDiscoverySendJoinRequest:
      // JOIN_ACK(NETWORK_ID) TX is in progress (or just completed).
      // As soon as rfState==Idle (TX done), immediately send JOIN_REQUEST.
      // No delay — fan has a short window after receiving JOIN_ACK(NETWORK_ID).
      if (this->rfState_ == RfStateIdle) {
        // rx and TX already switched to NETWORK_ID in JOIN_OPEN handler.
        // Fan rx=NETWORK_ID after JOIN_OPEN → our TX=NETWORK_ID reaches it.
        // FRAME_0B will arrive with TX=NETWORK_ID → our rx=NETWORK_ID receives it.
        ESP_LOGE(TAG, "rx=NETWORK_ID, TX=NETWORK_ID - sending JOIN_REQUEST immediately");

        RfFrame *const pJoinReq = (RfFrame *) this->_txFrame;
        (void) memset(this->_txFrame, 0, FAN_FRAMESIZE);
        pJoinReq->rx_type = FAN_TYPE_MAIN_CONTROL;  // 0x0E - pairing managed by MAIN_CONTROL
        pJoinReq->rx_id = this->config_.fan_main_unit_id;
        pJoinReq->tx_type = this->config_.fan_my_device_type;
        pJoinReq->tx_id = this->config_.fan_my_device_id;
        pJoinReq->ttl = FAN_TTL;
        pJoinReq->command = FAN_NETWORK_JOIN_REQUEST;
        pJoinReq->parameter_count = sizeof(RfPayloadNetworkJoinRequest);
        pJoinReq->payload.networkJoinRequest.networkId = this->config_.fan_networkId;

        this->pref_.save(&this->config_);
        this->config_loaded_ = true;

        ESP_LOGE(TAG, "Sending JOIN_REQUEST to MAIN_CONTROL(0x0E) id=0x%02X network=0x%08X",
                 this->config_.fan_main_unit_id, this->config_.fan_networkId);

        this->startTransmit(this->_txFrame, FAN_TX_RETRIES, [this]() {
          ESP_LOGW(TAG, "JOIN_REQUEST timeout - no FRAME_0B received, going Idle");
          this->state_ = StateIdle;
        });
        this->state_ = StateDiscoveryWaitForJoinResponse;
      }
      break;

    case StateStartDiscovery:
      deviceId = this->createDeviceID();
      this->discoveryStart(deviceId);

      // For now just set TX
      break;

    case StateIdle:
      // Handle pending speed changes
      if (newSetting == true) {
        this->setSpeed(newSpeed, newTimer);
      }
      // State is restored via ESPHome restore_mode and updated when
      // we receive STATUS_BROADCAST or SETSPEED from physical controls
      break;

    case StateWaitSetSpeedConfirm:
      if (this->rfState_ == RfStateIdle) {
        // When done, return to idle
        this->state_ = StateIdle;
      }

    default:
      break;
  }
}

void ZehnderRF::rfHandleReceived(const uint8_t *const pData, const uint8_t dataLength) {
  const RfFrame *const pResponse = (RfFrame *) pData;
  RfFrame *const pTxFrame = (RfFrame *) this->_txFrame;  // frame helper
  nrf905::Config rfConfig;

  // === ENHANCED DEBUG LOGGING ===
  ESP_LOGI(TAG, "========================================");
  ESP_LOGI(TAG, "RF Frame Received (state: 0x%02X)", this->state_);

  // Device type names
  const char* rx_type_name = "UNKNOWN";
  switch(pResponse->rx_type) {
    case FAN_TYPE_BROADCAST: rx_type_name = "BROADCAST"; break;
    case FAN_TYPE_MAIN_UNIT: rx_type_name = "MAIN_UNIT"; break;
    case FAN_TYPE_REMOTE_CONTROL: rx_type_name = "REMOTE"; break;
    case FAN_TYPE_MAIN_CONTROL: rx_type_name = "MAIN_CONTROL"; break;
    case FAN_TYPE_RF_REMOTE: rx_type_name = "RF_REMOTE"; break;
    case FAN_TYPE_CO2_SENSOR: rx_type_name = "CO2_SENSOR"; break;
  }
  const char* tx_type_name = "UNKNOWN";
  switch(pResponse->tx_type) {
    case FAN_TYPE_BROADCAST: tx_type_name = "BROADCAST"; break;
    case FAN_TYPE_MAIN_UNIT: tx_type_name = "MAIN_UNIT"; break;
    case FAN_TYPE_REMOTE_CONTROL: tx_type_name = "REMOTE"; break;
    case FAN_TYPE_MAIN_CONTROL: tx_type_name = "MAIN_CONTROL"; break;
    case FAN_TYPE_RF_REMOTE: tx_type_name = "RF_REMOTE"; break;
    case FAN_TYPE_CO2_SENSOR: tx_type_name = "CO2_SENSOR"; break;
  }

  ESP_LOGI(TAG, "  RX: 0x%02X (%s) ID=0x%02X", pResponse->rx_type, rx_type_name, pResponse->rx_id);
  ESP_LOGI(TAG, "  TX: 0x%02X (%s) ID=0x%02X", pResponse->tx_type, tx_type_name, pResponse->tx_id);
  ESP_LOGI(TAG, "  TTL: 0x%02X | Command: 0x%02X | Param Count: %d",
           pResponse->ttl, pResponse->command, pResponse->parameter_count);

  // Log command name
  const char* cmd_name = "UNKNOWN";
  switch(pResponse->command) {
    case FAN_FRAME_SETVOLTAGE: cmd_name = "SETVOLTAGE"; break;
    case FAN_FRAME_SETSPEED: cmd_name = "SETSPEED"; break;
    case FAN_FRAME_SETTIMER_OLD: cmd_name = "SETTIMER_OLD"; break;
    case FAN_NETWORK_JOIN_REQUEST: cmd_name = "JOIN_REQUEST"; break;
    case FAN_FRAME_SETSPEED_REPLY: cmd_name = "SETSPEED_REPLY"; break;
    case FAN_NETWORK_JOIN_OPEN: cmd_name = "JOIN_OPEN"; break;
    case FAN_TYPE_FAN_SETTINGS: cmd_name = "FAN_SETTINGS"; break;
    case FAN_FRAME_0B: cmd_name = "FRAME_0B"; break;
    case FAN_NETWORK_JOIN_ACK: cmd_name = "JOIN_ACK"; break;
    case FAN_TYPE_QUERY_NETWORK: cmd_name = "QUERY_NETWORK"; break;
    case FAN_TYPE_QUERY_DEVICE: cmd_name = "QUERY_DEVICE"; break;
    case FAN_FRAME_SETTIMER: cmd_name = "SETTIMER"; break;
    case FAN_FRAME_STATUS_BROADCAST: cmd_name = "STATUS_BROADCAST"; break;
    case FAN_FRAME_SETVOLTAGE_REPLY: cmd_name = "SETVOLTAGE_REPLY"; break;
  }
  ESP_LOGI(TAG, "  Command Name: %s", cmd_name);

  // Log parameters as hex
  if(pResponse->parameter_count > 0) {
    ESP_LOGI(TAG, "  Parameters [%d]: %02X %02X %02X %02X %02X %02X %02X %02X %02X",
             pResponse->parameter_count,
             pResponse->payload.parameters[0], pResponse->payload.parameters[1],
             pResponse->payload.parameters[2], pResponse->payload.parameters[3],
             pResponse->payload.parameters[4], pResponse->payload.parameters[5],
             pResponse->payload.parameters[6], pResponse->payload.parameters[7],
             pResponse->payload.parameters[8]);
  }
  ESP_LOGI(TAG, "========================================");
  // === END ENHANCED DEBUG LOGGING ===

  // === GLOBAL FRAME HANDLERS (process regardless of state) ===
  // Handle STATUS_BROADCAST (0x15) - broadcasted by MAIN_CONTROL after every status change
  if (pResponse->command == FAN_FRAME_STATUS_BROADCAST) {
    // STATUS_BROADCAST format: [0x10] [voltage %] [timer on/off]
    // Example: 10 5A 01 = 90% voltage, timer ON
    if (pResponse->parameter_count >= 3) {
      uint8_t voltage_percent = pResponse->payload.parameters[1];
      uint8_t timer_on = pResponse->payload.parameters[2];

      ESP_LOGI(TAG, "STATUS_BROADCAST: Voltage=%d%%, Timer=%s",
               voltage_percent, timer_on ? "ON" : "OFF");

      // NOTE: STATUS_BROADCAST only shows current voltage, not target preset
      // The voltage may be transitional (ramping up/down) so mapping is unreliable
      // We rely on SETSPEED broadcasts and FAN_SETTINGS for accurate state
      // Just log this for information, don't update state
      ESP_LOGD(TAG, "  (Not updating state - voltage %d%% may be transitional)", voltage_percent);
    }
    return;  // Don't process in state machine
  }

  // Handle FAN_SETTINGS (0x07) - reply from MAIN_UNIT with current settings
  if (pResponse->command == FAN_TYPE_FAN_SETTINGS) {
    // FAN_SETTINGS format: [target preset] [current voltage %] [unknown] [timer related]
    // Example: 04 32 00 04 = Target MAX (4), currently at 50%, ramping up
    if (pResponse->parameter_count >= 2) {
      uint8_t target_preset = pResponse->payload.parameters[0];
      uint8_t current_voltage = pResponse->payload.parameters[1];

      ESP_LOGI(TAG, "FAN_SETTINGS: Target preset=%d, Current voltage=%d%%",
               target_preset, current_voltage);

      // IMPORTANT: Use TARGET preset, not current voltage!
      // The fan may still be ramping up/down, but we want to show the target state
      // Map Zehnder preset to HA speed (DIRECT 1:1):
      // Preset 0 → OFF
      // Preset 1 → Speed 1 (25%)
      // Preset 2 → Speed 2 (50%)
      // Preset 3 → Speed 3 (75%)
      // Preset 4 → Speed 4 (100%)

      // Update fan state based on target preset (what the fan is moving towards)
      bool new_state = (target_preset > 0);  // Preset 0 = OFF
      uint8_t new_speed = target_preset;  // Direct 1:1: preset 1-4 → HA speed 1-4

      if (this->speed != new_speed || this->state != new_state) {
        ESP_LOGI(TAG, "Updating fan state from FAN_SETTINGS: preset %d → speed %d (ON)",
                 target_preset, new_speed);
        this->state = new_state;
        this->speed = new_speed;
        this->publish_state();
      }
    }
    // Continue processing in state machine (might be waiting for this response)
  }

  // Handle SETSPEED broadcasts (0x02) - from MAIN_CONTROL to all devices
  // These are broadcasted when physical remote or wired panel changes speed

  // DEBUG: Log what we're checking for SETSPEED broadcasts
  if (pResponse->command == FAN_FRAME_SETSPEED) {
    ESP_LOGI(TAG, "DEBUG SETSPEED: command=0x%02X (MATCH), rx_type=0x%02X (need 0x%02X), rx_id=0x%02X (need 0x00)",
             pResponse->command, pResponse->rx_type, FAN_TYPE_MAIN_UNIT, pResponse->rx_id);
    ESP_LOGI(TAG, "DEBUG SETSPEED: Condition check: rx_type_match=%s, rx_id_match=%s",
             (pResponse->rx_type == FAN_TYPE_MAIN_UNIT) ? "YES" : "NO",
             (pResponse->rx_id == 0x00) ? "YES" : "NO");
  }

  if (pResponse->command == FAN_FRAME_SETSPEED &&
      pResponse->rx_type == FAN_TYPE_MAIN_UNIT && pResponse->rx_id == 0x00) {
    // SETSPEED broadcast format: RX=MAIN_UNIT/0x00 (broadcast), TX=MAIN_CONTROL
    // Parameters: [speed preset] (0x00=AUTO, 0x01=LOW, 0x02=MEDIUM, 0x03=HIGH, 0x04=MAX)
    if (pResponse->parameter_count >= 1) {
      uint8_t speed_preset = pResponse->payload.parameters[0];

      ESP_LOGI(TAG, "!!! SETSPEED BROADCAST HANDLER TRIGGERED !!!");
      ESP_LOGI(TAG, "SETSPEED broadcast from MAIN_CONTROL: preset=%d", speed_preset);

      // Map preset to HA state/speed (DIRECT 1:1):
      // Preset 0 = OFF, Preset 1-4 = Speed 1-4
      bool new_state = (speed_preset > 0);  // Preset 0 = OFF
      uint8_t new_speed = speed_preset;  // Direct 1:1: preset 1-4 → HA speed 1-4

      // Update fan state if changed
      if (this->speed != new_speed || this->state != new_state) {
        ESP_LOGI(TAG, "Updating fan state from SETSPEED: preset %d → speed %d (%s)",
                 speed_preset, new_speed, new_state ? "ON" : "OFF");
        this->state = new_state;
        this->speed = new_speed;
        this->publish_state();
      } else {
        ESP_LOGI(TAG, "Fan state already matches SETSPEED broadcast (preset=%d → speed=%d)",
                 speed_preset, new_speed);
      }
    }
    return;  // Don't process broadcasts in state machine
  }
  // === END GLOBAL FRAME HANDLERS ===

  ESP_LOGD(TAG, "Current state: 0x%02X", this->state_);
  switch (this->state_) {
    case StateDiscoveryWaitForLinkRequest:
      ESP_LOGD(TAG, "DiscoverStateWaitForLinkRequest");
      switch (pResponse->command) {
        case FAN_NETWORK_JOIN_OPEN:  // Received linking request from main unit
          ESP_LOGE(TAG, "JOIN_OPEN received: type=0x%02X id=0x%02X network=0x%08X",
                   pResponse->tx_type, pResponse->tx_id, pResponse->payload.networkJoinOpen.networkId);

          this->rfComplete();  // Cancel current JOIN_ACK(LINK_ID) transmission

          // Save discovered fan info first (needed below)
          this->config_.fan_networkId = pResponse->payload.networkJoinOpen.networkId;
          this->config_.fan_main_unit_type = FAN_TYPE_MAIN_UNIT;  // 0x01 - SETSPEED target
          this->config_.fan_main_unit_id = pResponse->tx_id;

          // Fan switches its rx from LINK_ID → NETWORK_ID after sending JOIN_OPEN.
          // Switch our TX and rx to NETWORK_ID so:
          //   - JOIN_ACK(NETWORK_ID) reaches the fan (fan rx=NETWORK_ID now)
          //   - We receive FRAME_0B (fan TX=NETWORK_ID confirmed)
          {
            nrf905::Config rfCfg = this->rf_->getConfig();
            rfCfg.rx_address = this->config_.fan_networkId;
            this->rf_->updateConfig(&rfCfg, NULL);
            this->rf_->writeTxAddress(this->config_.fan_networkId);
          }
          ESP_LOGE(TAG, "JOIN_OPEN received - switching rx+TX to NETWORK_ID, sending JOIN_ACK(NETWORK_ID)");

          // Step 2 (cebbe06 sequence): send JOIN_ACK with NETWORK_ID in payload.
          // The fan needs to see this before it will accept JOIN_REQUEST and send FRAME_0B.
          // This tells the fan "I know your network address, I belong here".
          (void) memset(this->_txFrame, 0, FAN_FRAMESIZE);
          pTxFrame->rx_type = 0x04;  // Joining mode indicator (same as initial JOIN_ACK)
          pTxFrame->rx_id = 0x00;
          pTxFrame->tx_type = this->config_.fan_my_device_type;
          pTxFrame->tx_id = this->config_.fan_my_device_id;
          pTxFrame->ttl = FAN_TTL;
          pTxFrame->command = FAN_NETWORK_JOIN_ACK;
          pTxFrame->parameter_count = sizeof(RfPayloadNetworkJoinAck);
          pTxFrame->payload.networkJoinAck.networkId = this->config_.fan_networkId;  // NETWORK_ID in payload

          ESP_LOGE(TAG, "Sending JOIN_ACK(NETWORK_ID=0x%08X) - confirming we know the network",
                   this->config_.fan_networkId);

          // Send JOIN_ACK(NETWORK_ID) on LINK_ID channel (TX_ADDRESS=LINK_ID set above).
          // Fire-and-forget (retries=-1): TX complete → rfState=Idle immediately.
          // JOIN_REQUEST is sent directly from loop() in StateDiscoverySendJoinRequest
          // as soon as rfState==Idle. This avoids the 1000ms wait that caused the fan
          // to time out waiting for JOIN_REQUEST after receiving JOIN_ACK(NETWORK_ID).
          this->startTransmit(this->_txFrame, -1, NULL);

          // Move to intermediate state. loop() will fire JOIN_REQUEST when TX is done.
          this->state_ = StateDiscoverySendJoinRequest;
          break;

        default:
          ESP_LOGD(TAG, "Discovery: Received unknown frame type 0x%02X from ID 0x%02X", pResponse->command,
                   pResponse->tx_id);
          break;
      }
      break;

    case StateDiscoveryWaitForJoinResponse:
      ESP_LOGD(TAG, "DiscoverStateWaitForJoinResponse");
      switch (pResponse->command) {
        case FAN_FRAME_0B:
          ESP_LOGE(TAG, "FRAME_0B received! rx=0x%02X/0x%02X tx=0x%02X/0x%02X (expected rx=0x%02X/0x%02X tx=0x0E/0x%02X)",
                   pResponse->rx_type, pResponse->rx_id, pResponse->tx_type, pResponse->tx_id,
                   this->config_.fan_my_device_type, this->config_.fan_my_device_id, this->config_.fan_main_unit_id);
          if ((pResponse->rx_type == this->config_.fan_my_device_type) &&
              (pResponse->rx_id == this->config_.fan_my_device_id) &&
              (pResponse->tx_type == FAN_TYPE_MAIN_CONTROL) &&  // 0x0E - FRAME_0B from MAIN_CONTROL
              (pResponse->tx_id == this->config_.fan_main_unit_id)) {
            ESP_LOGE(TAG, "FRAME_0B filter PASSED - pairing confirmed by MAIN_CONTROL!");

            // Restore TX address to NETWORK_ID — pairing phase done, normal ops from here
            this->rf_->writeTxAddress(this->config_.fan_networkId);
            ESP_LOGE(TAG, "TX address restored to NETWORK_ID=0x%08X", this->config_.fan_networkId);

            this->rfComplete();

            (void) memset(this->_txFrame, 0, FAN_FRAMESIZE);  // Clear frame data

            pTxFrame->rx_type = FAN_TYPE_MAIN_UNIT;  // Set type to main unit
            pTxFrame->rx_id = pResponse->tx_id;      // Set ID to the ID of the main unit
            pTxFrame->tx_type = this->config_.fan_my_device_type;
            pTxFrame->tx_id = this->config_.fan_my_device_id;
            pTxFrame->ttl = FAN_TTL;
            pTxFrame->command = FAN_FRAME_0B;  // 0x0B acknowledge link successful
            pTxFrame->parameter_count = 0x00;  // No parameters

            // Send response frame
            this->startTransmit(this->_txFrame, FAN_TX_RETRIES, [this]() {
              ESP_LOGW(TAG, "Query Timeout");
              this->state_ = StateStartDiscovery;
            });

            this->state_ = StateDiscoveryJoinComplete;
          } else {
            ESP_LOGE(TAG, "Discovery: FRAME_0B type mismatch!");
            ESP_LOGE(TAG, "  Got:      rx_type=0x%02X rx_id=0x%02X tx_type=0x%02X tx_id=0x%02X",
                     pResponse->rx_type, pResponse->rx_id, pResponse->tx_type, pResponse->tx_id);
            ESP_LOGE(TAG, "  Expected: rx_type=0x%02X rx_id=0x%02X tx_type=0x%02X tx_id=0x%02X",
                     this->config_.fan_my_device_type, this->config_.fan_my_device_id,
                     this->config_.fan_main_unit_type, this->config_.fan_main_unit_id);
          }
          break;

        default:
          ESP_LOGE(TAG, "Discovery: Received unknown frame type 0x%02X from ID 0x%02X", pResponse->command,
                   pResponse->tx_id);
          break;
      }
      break;

    case StateDiscoveryJoinComplete:
      ESP_LOGD(TAG, "StateDiscoveryJoinComplete");
      switch (pResponse->command) {
        case FAN_TYPE_QUERY_NETWORK:
          if ((pResponse->rx_type == this->config_.fan_main_unit_type) &&
              (pResponse->rx_id == this->config_.fan_main_unit_id) &&
              (pResponse->tx_type == this->config_.fan_main_unit_type) &&
              (pResponse->tx_id == this->config_.fan_main_unit_id)) {
            ESP_LOGD(TAG, "Discovery: received network join success 0x0D");

            this->rfComplete();

            ESP_LOGE(TAG, "PAIRING COMPLETE - switching to NETWORK channel");
            this->config_.fan_main_unit_type = FAN_TYPE_MAIN_UNIT;
            this->pref_.save(&this->config_);
            this->config_loaded_ = true;

            // Now switch rx_address to NETWORK_ID for normal operation
            {
              nrf905::Config rfCfg = this->rf_->getConfig();
              rfCfg.rx_address = this->config_.fan_networkId;
              this->rf_->updateConfig(&rfCfg, NULL);
            }

            this->state_ = StateIdle;
          } else {
            ESP_LOGW(TAG, "Unexpected frame join reponse from Type 0x%02X ID 0x%02X", pResponse->tx_type,
                     pResponse->tx_id);
          }
          break;

        default:
          ESP_LOGE(TAG, "Discovery: Received unknown frame type 0x%02X from ID 0x%02X on network 0x%08X",
                   pResponse->command, pResponse->tx_id, this->config_.fan_networkId);
          break;
      }
      break;

    case StateWaitQueryResponse:
      if ((pResponse->rx_type == this->config_.fan_my_device_type) &&  // If type
          (pResponse->rx_id == this->config_.fan_my_device_id)) {      // and id match, it is for us
        switch (pResponse->command) {
          case FAN_TYPE_FAN_SETTINGS:
            ESP_LOGD(TAG, "Received fan settings; speed: 0x%02X voltage: %i timer: %i",
                     pResponse->payload.fanSettings.speed, pResponse->payload.fanSettings.voltage,
                     pResponse->payload.fanSettings.timer);

            this->rfComplete();

            this->state = pResponse->payload.fanSettings.speed > 0;
            this->speed = pResponse->payload.fanSettings.speed;
            this->publish_state();

            this->state_ = StateIdle;
            break;

          default:
            ESP_LOGD(TAG, "Received unexpected frame; type 0x%02X from ID 0x%02X", pResponse->command,
                     pResponse->tx_id);
            break;
        }
      } else {
        ESP_LOGD(TAG, "Received frame from unknown device; type 0x%02X from ID 0x%02X type 0x%02X", pResponse->command,
                 pResponse->tx_id, pResponse->tx_type);
      }
      break;

    case StateWaitSetSpeedResponse:
      ESP_LOGI(TAG, "StateWaitSetSpeedResponse: Processing frame (command=0x%02X, rx_type=0x%02X, rx_id=0x%02X)",
               pResponse->command, pResponse->rx_type, pResponse->rx_id);
      ESP_LOGI(TAG, "  Expecting: rx_type=0x%02X, rx_id=0x%02X",
               this->config_.fan_my_device_type, this->config_.fan_my_device_id);

      if ((pResponse->rx_type == this->config_.fan_my_device_type) &&  // If type
          (pResponse->rx_id == this->config_.fan_my_device_id)) {      // and id match, it is for us
        ESP_LOGI(TAG, "  Frame is addressed to us!");
        switch (pResponse->command) {
          case FAN_TYPE_FAN_SETTINGS:
            ESP_LOGI(TAG, "StateWaitSetSpeedResponse: Received FAN_SETTINGS confirmation");
            ESP_LOGD(TAG, "  Speed: 0x%02X, Voltage: %i%%, Timer: %i",
                     pResponse->payload.fanSettings.speed,
                     pResponse->payload.fanSettings.voltage,
                     pResponse->payload.fanSettings.timer);

            // Command successful! Cancel retries and return to Idle
            this->rfComplete();
            this->state_ = StateIdle;
            ESP_LOGI(TAG, "SetSpeed successful - returning to Idle state");
            break;

          case FAN_FRAME_SETSPEED_REPLY:
          case FAN_FRAME_SETVOLTAGE_REPLY:
            ESP_LOGI(TAG, "  Received SETSPEED_REPLY or SETVOLTAGE_REPLY (ignoring for now)");
            // this->rfComplete();

            // this->state_ = StateIdle;
            break;

          default:
            ESP_LOGI(TAG, "  Received unexpected frame type 0x%02X from ID 0x%02X", pResponse->command,
                     pResponse->tx_id);
            break;
        }
      } else {
        ESP_LOGI(TAG, "  Frame NOT addressed to us (rx_type=0x%02X/0x%02X, rx_id=0x%02X/0x%02X) - ignoring",
                 pResponse->rx_type, this->config_.fan_my_device_type,
                 pResponse->rx_id, this->config_.fan_my_device_id);
      }
      break;

    default:
      ESP_LOGD(TAG, "Received frame from unknown device in unknown state; type 0x%02X from ID 0x%02X type 0x%02X",
               pResponse->command, pResponse->tx_id, pResponse->tx_type);
      break;
  }
}

static uint8_t minmax(const uint8_t value, const uint8_t min, const uint8_t max) {
  if (value <= min) {
    return min;
  } else if (value >= max) {
    return max;
  } else {
    return value;
  }
}

uint8_t ZehnderRF::createDeviceID(void) {
  uint8_t random = (uint8_t) random_uint32();
  // Generate random device_id; don't use 0x00 and 0xFF

  // TODO: there's a 1 in 255 chance that the generated ID matches the ID of the main unit. Decide how to deal
  // withthis (some sort of ping discovery?)

  return minmax(random, 1, 0xFE);
}

void ZehnderRF::queryDevice(void) {
  RfFrame *const pFrame = (RfFrame *) this->_txFrame;  // frame helper

  ESP_LOGD(TAG, "Query device");

  this->lastFanQuery_ = millis();  // Update time

  // Clear frame data
  (void) memset(this->_txFrame, 0, FAN_FRAMESIZE);

  // Build frame
  pFrame->rx_type = this->config_.fan_main_unit_type;
  pFrame->rx_id = this->config_.fan_main_unit_id;
  pFrame->tx_type = this->config_.fan_my_device_type;
  pFrame->tx_id = this->config_.fan_my_device_id;
  pFrame->ttl = FAN_TTL;
  pFrame->command = FAN_TYPE_QUERY_DEVICE;
  pFrame->parameter_count = 0x00;  // No parameters

  this->startTransmit(this->_txFrame, FAN_TX_RETRIES, [this]() {
    ESP_LOGW(TAG, "Query Timeout");
    this->state_ = StateIdle;
  });

  this->state_ = StateWaitQueryResponse;
}

void ZehnderRF::setSpeed(const uint8_t paramSpeed, const uint8_t paramTimer) {
  RfFrame *const pFrame = (RfFrame *) this->_txFrame;  // frame helper
  uint8_t speed = paramSpeed;
  uint8_t timer = paramTimer;

  if (speed > this->speed_count_) {
    ESP_LOGW(TAG, "Requested speed too high (%u)", speed);
    speed = this->speed_count_;
  }

  ESP_LOGD(TAG, "Set speed: 0x%02X; Timer %u minutes", speed, timer);

  if (this->state_ == StateIdle) {
    (void) memset(this->_txFrame, 0, FAN_FRAMESIZE);  // Clear frame data

    // Build frame
    pFrame->rx_type = this->config_.fan_main_unit_type;
    pFrame->rx_id = 0x00;  // broadcast - MAIN_UNIT accepts commands to rx_id=0x00
    pFrame->tx_type = this->config_.fan_my_device_type;
    pFrame->tx_id = this->config_.fan_my_device_id;
    pFrame->ttl = FAN_TTL;

    if (timer == 0) {
      pFrame->command = FAN_FRAME_SETSPEED;
      pFrame->parameter_count = sizeof(RfPayloadFanSetSpeed);
      pFrame->payload.setSpeed.speed = speed;
    } else {
      pFrame->command = FAN_FRAME_SETTIMER;
      pFrame->parameter_count = sizeof(RfPayloadFanSetTimer);
      pFrame->payload.setTimer.speed = speed;
      pFrame->payload.setTimer.timer = timer;
    }

    this->startTransmit(this->_txFrame, FAN_TX_RETRIES, [this]() {
      ESP_LOGE(TAG, "!!! SET SPEED TIMEOUT - NO RESPONSE FROM FAN !!!");
      ESP_LOGE(TAG, "Returning to Idle state after timeout");
      this->state_ = StateIdle;
    });

    newSetting = false;
    this->state_ = StateWaitSetSpeedResponse;
  } else {
    ESP_LOGD(TAG, "Invalid state, I'm trying later again");
    newSpeed = speed;
    newTimer = timer;
    newSetting = true;
  }
}

void ZehnderRF::discoveryStart(const uint8_t deviceId) {
  RfFrame *const pFrame = (RfFrame *) this->_txFrame;  // frame helper
  nrf905::Config rfConfig;

  ESP_LOGD(TAG, "Start discovery with ID %u", deviceId);

  this->config_.fan_my_device_type = FAN_TYPE_RF_REMOTE;  // 0x0F - must match bathroom remote type
  this->config_.fan_my_device_id = deviceId;

  // Build frame
  (void) memset(this->_txFrame, 0, FAN_FRAMESIZE);  // Clear frame data

  // Set payload, available for linking
  pFrame->rx_type = 0x04;
  pFrame->rx_id = 0x00;
  pFrame->tx_type = this->config_.fan_my_device_type;
  pFrame->tx_id = this->config_.fan_my_device_id;
  pFrame->ttl = FAN_TTL;
  pFrame->command = FAN_NETWORK_JOIN_ACK;
  pFrame->parameter_count = sizeof(RfPayloadNetworkJoinAck);
  pFrame->payload.networkJoinAck.networkId = NETWORK_LINK_ID;

  // Switch rx_address to LINK_ID so we can receive JOIN_OPEN from fan
  // TX address stays NETWORK_ID (do not call writeTxAddress here)
  nrf905::Config rfCfg2 = this->rf_->getConfig();
  rfCfg2.rx_address = NETWORK_LINK_ID;
  this->rf_->updateConfig(&rfCfg2, NULL);

  this->startTransmit(this->_txFrame, FAN_TX_RETRIES, [this]() {
    ESP_LOGW(TAG, "Start discovery timeout");
    this->state_ = StateStartDiscovery;
  });

  // Update state
  this->state_ = StateDiscoveryWaitForLinkRequest;
}

Result ZehnderRF::startTransmit(const uint8_t *const pData, const int8_t rxRetries,
                                const std::function<void(void)> callback) {
  Result result = ResultOk;
  unsigned long startTime;
  bool busy = true;

  if (this->rfState_ != RfStateIdle) {
    ESP_LOGW(TAG, "TX still ongoing");
    result = ResultBusy;
  } else {
    this->onReceiveTimeout_ = callback;
    this->retries_ = rxRetries;

    // Write data to RF
    // if (pData != NULL) {  // If frame given, load it in the nRF. Else use previous TX payload
    // ESP_LOGD(TAG, "Write payload");
    this->rf_->writeTxPayload(pData, FAN_FRAMESIZE);  // Use framesize
    // }

    this->rfState_ = RfStateWaitAirwayFree;
    this->airwayFreeWaitTime_ = millis();
  }

  return result;
}

void ZehnderRF::rfComplete(void) {
  this->retries_ = -1;  // Disable this->retries_
  this->rfState_ = RfStateIdle;
}

void ZehnderRF::rfHandler(void) {
  switch (this->rfState_) {
    case RfStateIdle:
      break;

    case RfStateWaitAirwayFree:
      if ((millis() - this->airwayFreeWaitTime_) > 5000) {
        ESP_LOGW(TAG, "Airway too busy, giving up");
        this->rfState_ = RfStateIdle;

        if (this->onReceiveTimeout_ != NULL) {
          this->onReceiveTimeout_();
        }
      } else if (this->rf_->airwayBusy() == false) {
        ESP_LOGD(TAG, "Start TX");
        this->rf_->startTx(FAN_TX_FRAMES, nrf905::Receive);  // After transmit, wait for response

        this->rfState_ = RfStateTxBusy;
      }
      break;

    case RfStateTxBusy:
      break;

    case RfStateRxWait:
      if ((this->retries_ >= 0) && ((millis() - this->msgSendTime_) > FAN_REPLY_TIMEOUT)) {
        ESP_LOGI(TAG, "RfStateRxWait: Receive timeout (waited %ums)", millis() - this->msgSendTime_);

        if (this->retries_ > 0) {
          --this->retries_;
          ESP_LOGI(TAG, "  No response received, retrying... (retries left: %u)", this->retries_);

          this->rfState_ = RfStateWaitAirwayFree;
          this->airwayFreeWaitTime_ = millis();
        } else if (this->retries_ == 0) {
          // Oh oh, ran out of options
          ESP_LOGE(TAG, "  !!! ALL RETRIES EXHAUSTED - CALLING TIMEOUT CALLBACK !!!");
          ESP_LOGE(TAG, "  No messages received after %d retries, giving up now...", FAN_TX_RETRIES);
          // Set Idle BEFORE calling callback so callback can call startTransmit()
          this->rfState_ = RfStateIdle;

          if (this->onReceiveTimeout_ != NULL) {
            this->onReceiveTimeout_();
          } else {
            ESP_LOGE(TAG, "  WARNING: No timeout callback registered!");
          }
        }
      }
      break;

    default:
      break;
  }
}

}  // namespace zehnder
}  // namespace esphome
