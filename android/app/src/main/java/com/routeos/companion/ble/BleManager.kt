package com.routeos.companion.ble

import android.annotation.SuppressLint
import android.bluetooth.*
import android.bluetooth.le.*
import android.content.Context
import android.os.ParcelUuid
import com.routeos.companion.model.ScannedDevice
import kotlinx.coroutines.*
import kotlinx.coroutines.channels.awaitClose
import kotlinx.coroutines.flow.*
import java.util.UUID

@SuppressLint("MissingPermission")
class BleManager(private val context: Context) {

    private val adapter: BluetoothAdapter =
        (context.getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager).adapter

    private val _scannedDevices = MutableStateFlow<List<ScannedDevice>>(emptyList())
    val scannedDevices: StateFlow<List<ScannedDevice>> = _scannedDevices.asStateFlow()

    private val _isScanning = MutableStateFlow(false)
    val isScanning: StateFlow<Boolean> = _isScanning.asStateFlow()

    private var scanJob: Job? = null

    // ─── Scanning ─────────────────────────────────────────────────────────────

    fun startScan(scope: CoroutineScope) {
        if (_isScanning.value) return
        _scannedDevices.value = emptyList()

        val seen = mutableMapOf<String, ScannedDevice>()
        val scanner = adapter.bluetoothLeScanner ?: return

        val settings = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
            .build()

        // Filter for any device — we filter by name ourselves so we also see
        // devices that don't broadcast a service UUID
        val callback = object : ScanCallback() {
            override fun onScanResult(callbackType: Int, result: ScanResult) {
                val name = result.device.name ?: return
                if (!name.startsWith(BleConstants.DEVICE_NAME_PREFIX)) return
                val device = ScannedDevice(name, result.device.address, result.rssi)
                seen[result.device.address] = device
                _scannedDevices.value = seen.values.sortedByDescending { it.rssi }
            }
        }

        scanJob = scope.launch {
            _isScanning.value = true
            scanner.startScan(null, settings, callback)
            delay(BleConstants.SCAN_PERIOD_MS)
            scanner.stopScan(callback)
            _isScanning.value = false
        }
    }

    fun stopScan() {
        scanJob?.cancel()
        _isScanning.value = false
    }

    // ─── GATT connection ──────────────────────────────────────────────────────

    fun connect(address: String): BleProvisioner {
        val device = adapter.getRemoteDevice(address)
        return BleProvisioner(context, device)
    }
}
