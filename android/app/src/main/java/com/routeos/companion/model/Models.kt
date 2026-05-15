package com.routeos.companion.model

data class ScannedDevice(
    val name: String,
    val address: String,
    val rssi: Int,
)

data class DeviceStatus(
    val wifiConnected: Boolean = false,
    val wifiSsid: String = "",
    val batteryVoltage: Float = 0f,
    val batteryPercent: Int = 0,
    val firmwareVersion: String = "",
    val deviceCode: String = "",
    val configured: Boolean = false,
    val apMode: Int = 0,
)

data class ProvisionConfig(
    val wifiSsid: String = "",
    val wifiPass: String = "",
    val deviceCode: String = "",
    val apiUrl: String = "http://192.168.1.100:4003/api/devices",
    val reportIntervalSec: Int = 900,
    val binHeightCm: Float = 100f,
)
