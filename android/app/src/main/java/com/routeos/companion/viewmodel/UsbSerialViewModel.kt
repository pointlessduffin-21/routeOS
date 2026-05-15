package com.routeos.companion.viewmodel

import android.app.Application
import androidx.lifecycle.AndroidViewModel
import com.routeos.companion.usb.UsbSerialConfig
import com.routeos.companion.usb.UsbSerialManager
import com.routeos.companion.usb.UsbState
import kotlinx.coroutines.flow.StateFlow

class UsbSerialViewModel(app: Application) : AndroidViewModel(app) {

    private val manager = UsbSerialManager(app.applicationContext)

    val state: StateFlow<UsbState>        = manager.state
    val log: StateFlow<List<String>>      = manager.log
    val connectedDevice: StateFlow<String?> = manager.connectedDevice

    fun findDevices() = manager.findDevices()

    fun connect(baudRate: Int = 115200) =
        manager.connect(UsbSerialConfig(baudRate = baudRate))

    fun disconnect() = manager.disconnect()

    fun send(text: String) = manager.send(text)

    fun clearLog() = manager.clearLog()

    override fun onCleared() {
        manager.release()
        super.onCleared()
    }
}
