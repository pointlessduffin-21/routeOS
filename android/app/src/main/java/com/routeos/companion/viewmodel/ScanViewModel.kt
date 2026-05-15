package com.routeos.companion.viewmodel

import android.app.Application
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.routeos.companion.ble.BleManager
import com.routeos.companion.model.ScannedDevice
import kotlinx.coroutines.flow.StateFlow

class ScanViewModel(app: Application) : AndroidViewModel(app) {

    private val bleManager = BleManager(app.applicationContext)

    val scannedDevices: StateFlow<List<ScannedDevice>> = bleManager.scannedDevices
    val isScanning: StateFlow<Boolean> = bleManager.isScanning

    fun startScan() = bleManager.startScan(viewModelScope)
    fun stopScan()  = bleManager.stopScan()

    fun createProvisioner(address: String) = bleManager.connect(address)

    override fun onCleared() {
        bleManager.stopScan()
        super.onCleared()
    }
}
