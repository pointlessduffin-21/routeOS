#ifndef ECOROUTE_CONFIG_H
#define ECOROUTE_CONFIG_H

#include <Arduino.h>

// ─── Firmware Version ───────────────────────────────────────────────────────
#define FIRMWARE_VERSION "1.0.0"

// ─── HC-SR04 Ultrasonic Sensor ──────────────────────────────────────────────
#define HCSR04_TRIG_PIN     GPIO_NUM_5
#define HCSR04_ECHO_PIN     GPIO_NUM_18

// ─── Battery Voltage ADC ────────────────────────────────────────────────────
// Voltage divider: R1=100k, R2=100k → max 4.2V reads as ~2.1V on ADC
// Uses ADC1 (ADC2 conflicts with WiFi)
#define BATTERY_ADC_PIN         GPIO_NUM_34   // ADC1_CH6
#define VOLTAGE_DIVIDER_RATIO   2.0f
#define ADC_VREF                3.3f

// ─── BLE Configuration ─────────────────────────────────────────────────────
#define BLE_DEVICE_NAME_PREFIX  "ECO-BIN-"

// BLE GATT UUIDs
#define SERVICE_UUID            "ec010001-b100-4ec0-b100-ec00000e0000"
#define CHAR_WIFI_SSID_UUID     "ec010002-b100-4ec0-b100-ec00000e0000"
#define CHAR_WIFI_PASS_UUID     "ec010003-b100-4ec0-b100-ec00000e0000"
#define CHAR_DEVICE_CODE_UUID   "ec010004-b100-4ec0-b100-ec00000e0000"
#define CHAR_API_URL_UUID       "ec010005-b100-4ec0-b100-ec00000e0000"
#define CHAR_INTERVAL_UUID      "ec010006-b100-4ec0-b100-ec00000e0000"
#define CHAR_BIN_HEIGHT_UUID    "ec010007-b100-4ec0-b100-ec00000e0000"
#define CHAR_STATUS_UUID        "ec010008-b100-4ec0-b100-ec00000e0000"
#define CHAR_COMMAND_UUID       "ec010009-b100-4ec0-b100-ec00000e0000"

// BLE Commands
#define CMD_SAVE_AND_RESTART    0x01
#define CMD_FORCE_REPORT        0x02
#define CMD_FACTORY_RESET       0x03
#define CMD_ENABLE_AP           0x04
#define CMD_DISABLE_AP          0x05
#define CMD_SET_AP_HIDDEN       0x06
#define CMD_SET_AP_ON_DEMAND    0x07
#define CMD_SET_AP_ALWAYS       0x08

// ─── AP Broadcast Modes ────────────────────────────────────────────────────
// ALWAYS:    AP always visible (default for unconfigured devices)
// HIDDEN:    AP running but SSID hidden; only direct connection or BLE discovery
// ON_DEMAND: AP off by default; BLE command or physical button enables temporarily
#define AP_MODE_ALWAYS          0
#define AP_MODE_HIDDEN          1
#define AP_MODE_ON_DEMAND       2

#define AP_ON_DEMAND_TIMEOUT_MS 300000  // 5 minutes auto-disable

// ─── MQTT Broker ────────────────────────────────────────────────────────────
#define MQTT_BROKER_HOST        "109.123.238.215"
#define MQTT_BROKER_PORT        1883
#define MQTT_TOPIC_PREFIX       "ecoroute/trash_can/"
#define MQTT_CONNECT_TIMEOUT_MS 10000

// ─── Default Configuration ──────────────────────────────────────────────────
#define DEFAULT_DEVICE_CODE         "ECO-BIN-001"
#define DEFAULT_REPORT_INTERVAL_SEC  900      // 15 minutes
#define DEFAULT_BIN_HEIGHT_CM        100.0f
#define DEFAULT_API_URL              "http://10.0.2.2:3000/api/v1/device/telemetry"
#define DEFAULT_API_KEY              "ecoroute-device-key-change-in-production"

// ─── Timing ─────────────────────────────────────────────────────────────────
#define WIFI_CONNECT_TIMEOUT_MS      15000
#define HTTP_TIMEOUT_MS              10000
#define BLE_ADVERTISING_TIMEOUT_SEC  300    // 5 minutes

// ─── Sensor Sampling ────────────────────────────────────────────────────────
#define NUM_DISTANCE_SAMPLES    5
#define SAMPLE_DELAY_MS         60
#define MAX_DISTANCE_CM         400.0f    // HC-SR04 max range
#define ECHO_TIMEOUT_US         30000     // ~5m max distance

// ─── Status LED ─────────────────────────────────────────────────────────────
#define LED_PIN                 GPIO_NUM_2  // Onboard LED

#endif // ECOROUTE_CONFIG_H
