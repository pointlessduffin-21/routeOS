package com.routeos.companion.ble

import android.annotation.SuppressLint
import android.bluetooth.*
import android.content.Context
import android.os.Build
import com.routeos.companion.model.DeviceStatus
import com.routeos.companion.model.ProvisionConfig
import kotlinx.coroutines.*
import kotlinx.coroutines.flow.*
import org.json.JSONObject
import java.util.UUID

sealed class ProvisionState {
    object Idle : ProvisionState()
    object Connecting : ProvisionState()
    object Connected : ProvisionState()
    data class Writing(val step: String) : ProvisionState()
    data class Done(val message: String) : ProvisionState()
    data class Error(val message: String) : ProvisionState()
}

@SuppressLint("MissingPermission")
class BleProvisioner(
    private val context: Context,
    private val device: BluetoothDevice,
) {
    private val _state = MutableStateFlow<ProvisionState>(ProvisionState.Idle)
    val state: StateFlow<ProvisionState> = _state.asStateFlow()

    private val _deviceStatus = MutableStateFlow<DeviceStatus?>(null)
    val deviceStatus: StateFlow<DeviceStatus?> = _deviceStatus.asStateFlow()

    private var gatt: BluetoothGatt? = null
    private val writeQueue: ArrayDeque<suspend () -> Unit> = ArrayDeque()

    // Deferred to synchronize characteristic operations
    private var operationDeferred: CompletableDeferred<Boolean>? = null

    private val gattCallback = object : BluetoothGattCallback() {

        override fun onConnectionStateChange(gatt: BluetoothGatt, status: Int, newState: Int) {
            when (newState) {
                BluetoothProfile.STATE_CONNECTED -> {
                    _state.value = ProvisionState.Connected
                    gatt.discoverServices()
                }
                BluetoothProfile.STATE_DISCONNECTED -> {
                    if (_state.value !is ProvisionState.Done) {
                        _state.value = ProvisionState.Error("Disconnected (status=$status)")
                    }
                    this@BleProvisioner.gatt = null
                }
            }
        }

        override fun onServicesDiscovered(gatt: BluetoothGatt, status: Int) {
            if (status != BluetoothGatt.GATT_SUCCESS) {
                _state.value = ProvisionState.Error("Service discovery failed ($status)")
                return
            }
            // Enable status notifications
            enableNotifications(gatt)
        }

        override fun onCharacteristicWrite(
            gatt: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            status: Int
        ) {
            operationDeferred?.complete(status == BluetoothGatt.GATT_SUCCESS)
        }

        @Deprecated("Deprecated in API 33")
        override fun onCharacteristicChanged(
            gatt: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic
        ) {
            if (characteristic.uuid == UUID.fromString(BleConstants.CHAR_STATUS_UUID)) {
                parseStatus(characteristic.value)
            }
        }

        override fun onCharacteristicChanged(
            gatt: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            value: ByteArray
        ) {
            if (characteristic.uuid == UUID.fromString(BleConstants.CHAR_STATUS_UUID)) {
                parseStatus(value)
            }
        }

        override fun onDescriptorWrite(
            gatt: BluetoothGatt,
            descriptor: BluetoothGattDescriptor,
            status: Int
        ) {
            operationDeferred?.complete(status == BluetoothGatt.GATT_SUCCESS)
        }
    }

    private fun parseStatus(bytes: ByteArray) {
        try {
            val json = JSONObject(String(bytes))
            _deviceStatus.value = DeviceStatus(
                wifiConnected   = json.optBoolean("wifi_connected", false),
                wifiSsid        = json.optString("ssid", ""),
                batteryVoltage  = json.optDouble("battery_voltage", 0.0).toFloat(),
                batteryPercent  = json.optInt("battery_percent", 0),
                firmwareVersion = json.optString("firmware", ""),
                deviceCode      = json.optString("device_code", ""),
                configured      = json.optBoolean("configured", false),
                apMode          = json.optInt("ap_mode", 0),
            )
        } catch (_: Exception) {}
    }

    private fun enableNotifications(gatt: BluetoothGatt) {
        val svc  = gatt.getService(UUID.fromString(BleConstants.SERVICE_UUID)) ?: return
        val char = svc.getCharacteristic(UUID.fromString(BleConstants.CHAR_STATUS_UUID)) ?: return
        gatt.setCharacteristicNotification(char, true)
        val descriptor = char.getDescriptor(UUID.fromString(BleConstants.CLIENT_CONFIG_UUID))
        descriptor?.let {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                gatt.writeDescriptor(it, BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE)
            } else {
                @Suppress("DEPRECATION")
                it.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
                gatt.writeDescriptor(it)
            }
        }
    }

    fun connect() {
        _state.value = ProvisionState.Connecting
        gatt = device.connectGatt(context, false, gattCallback, BluetoothDevice.TRANSPORT_LE)
    }

    // ─── Provision — writes all config fields then saves ─────────────────────

    fun provision(config: ProvisionConfig, scope: CoroutineScope) {
        scope.launch(Dispatchers.IO) {
            val g = gatt ?: run {
                _state.value = ProvisionState.Error("Not connected"); return@launch
            }
            val svc = g.getService(UUID.fromString(BleConstants.SERVICE_UUID)) ?: run {
                _state.value = ProvisionState.Error("routeOS service not found"); return@launch
            }

            suspend fun writeChar(uuid: String, value: String, label: String): Boolean {
                _state.value = ProvisionState.Writing(label)
                val char = svc.getCharacteristic(UUID.fromString(uuid)) ?: return false
                operationDeferred = CompletableDeferred()
                val bytes = value.toByteArray(Charsets.UTF_8)
                val ok = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                    g.writeCharacteristic(char, bytes, BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT)
                    true
                } else {
                    @Suppress("DEPRECATION")
                    char.value = bytes
                    g.writeCharacteristic(char)
                }
                if (!ok) return false
                return withTimeoutOrNull(BleConstants.GATT_TIMEOUT_MS) {
                    operationDeferred!!.await()
                } ?: false
            }

            suspend fun writeCommand(cmd: Byte, label: String): Boolean {
                _state.value = ProvisionState.Writing(label)
                val char = svc.getCharacteristic(UUID.fromString(BleConstants.CHAR_COMMAND_UUID)) ?: return false
                operationDeferred = CompletableDeferred()
                val ok = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                    g.writeCharacteristic(char, byteArrayOf(cmd), BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT)
                    true
                } else {
                    @Suppress("DEPRECATION")
                    char.value = byteArrayOf(cmd)
                    g.writeCharacteristic(char)
                }
                if (!ok) return false
                return withTimeoutOrNull(BleConstants.GATT_TIMEOUT_MS) {
                    operationDeferred!!.await()
                } ?: false
            }

            var ok = writeChar(BleConstants.CHAR_WIFI_SSID_UUID, config.wifiSsid, "WiFi SSID")
            if (!ok) { _state.value = ProvisionState.Error("Failed to write WiFi SSID"); return@launch }

            ok = writeChar(BleConstants.CHAR_WIFI_PASS_UUID, config.wifiPass, "WiFi password")
            if (!ok) { _state.value = ProvisionState.Error("Failed to write WiFi password"); return@launch }

            if (config.deviceCode.isNotBlank()) {
                ok = writeChar(BleConstants.CHAR_DEVICE_CODE_UUID, config.deviceCode, "Device code")
                if (!ok) { _state.value = ProvisionState.Error("Failed to write device code"); return@launch }
            }

            if (config.apiUrl.isNotBlank()) {
                ok = writeChar(BleConstants.CHAR_API_URL_UUID, config.apiUrl, "API URL")
                if (!ok) { _state.value = ProvisionState.Error("Failed to write API URL"); return@launch }
            }

            ok = writeChar(BleConstants.CHAR_INTERVAL_UUID, config.reportIntervalSec.toString(), "Report interval")
            if (!ok) { _state.value = ProvisionState.Error("Failed to write interval"); return@launch }

            ok = writeChar(BleConstants.CHAR_BIN_HEIGHT_UUID, config.binHeightCm.toString(), "Bin height")
            if (!ok) { _state.value = ProvisionState.Error("Failed to write bin height"); return@launch }

            ok = writeCommand(BleConstants.CMD_SAVE_AND_RESTART, "Saving & restarting…")
            if (!ok) { _state.value = ProvisionState.Error("Failed to send save command"); return@launch }

            _state.value = ProvisionState.Done("Device provisioned — rebooting")
            delay(1000)
            disconnect()
        }
    }

    fun sendCommand(cmd: Byte, scope: CoroutineScope) {
        scope.launch(Dispatchers.IO) {
            val g = gatt ?: return@launch
            val svc  = g.getService(UUID.fromString(BleConstants.SERVICE_UUID)) ?: return@launch
            val char = svc.getCharacteristic(UUID.fromString(BleConstants.CHAR_COMMAND_UUID)) ?: return@launch
            operationDeferred = CompletableDeferred()
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                g.writeCharacteristic(char, byteArrayOf(cmd), BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT)
            } else {
                @Suppress("DEPRECATION")
                char.value = byteArrayOf(cmd)
                g.writeCharacteristic(char)
            }
            withTimeoutOrNull(BleConstants.GATT_TIMEOUT_MS) { operationDeferred!!.await() }
        }
    }

    fun disconnect() {
        gatt?.disconnect()
        gatt?.close()
        gatt = null
    }
}
