package com.routeos.companion.ble

// ─── Matches config.h on the ESP32 firmware ──────────────────────────────────

object BleConstants {
    const val DEVICE_NAME_PREFIX = "ECO-BIN-"

    const val SERVICE_UUID         = "ec010001-b100-4ec0-b100-ec00000e0000"
    const val CHAR_WIFI_SSID_UUID  = "ec010002-b100-4ec0-b100-ec00000e0000"
    const val CHAR_WIFI_PASS_UUID  = "ec010003-b100-4ec0-b100-ec00000e0000"
    const val CHAR_DEVICE_CODE_UUID= "ec010004-b100-4ec0-b100-ec00000e0000"
    const val CHAR_API_URL_UUID    = "ec010005-b100-4ec0-b100-ec00000e0000"
    const val CHAR_INTERVAL_UUID   = "ec010006-b100-4ec0-b100-ec00000e0000"
    const val CHAR_BIN_HEIGHT_UUID = "ec010007-b100-4ec0-b100-ec00000e0000"
    const val CHAR_STATUS_UUID     = "ec010008-b100-4ec0-b100-ec00000e0000"
    const val CHAR_COMMAND_UUID    = "ec010009-b100-4ec0-b100-ec00000e0000"

    // BLE descriptor UUID for notifications
    const val CLIENT_CONFIG_UUID   = "00002902-0000-1000-8000-00805f9b34fb"

    // Commands (match firmware CMD_* defines)
    const val CMD_SAVE_AND_RESTART: Byte = 0x01
    const val CMD_FORCE_REPORT:     Byte = 0x02
    const val CMD_FACTORY_RESET:    Byte = 0x03
    const val CMD_ENABLE_AP:        Byte = 0x04
    const val CMD_DISABLE_AP:       Byte = 0x05
    const val CMD_SET_AP_HIDDEN:    Byte = 0x06
    const val CMD_SET_AP_ON_DEMAND: Byte = 0x07
    const val CMD_SET_AP_ALWAYS:    Byte = 0x08

    const val SCAN_PERIOD_MS = 10_000L
    const val GATT_TIMEOUT_MS = 15_000L
}
