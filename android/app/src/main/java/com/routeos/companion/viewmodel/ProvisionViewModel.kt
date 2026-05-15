package com.routeos.companion.viewmodel

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.routeos.companion.ble.BleConstants
import com.routeos.companion.ble.BleProvisioner
import com.routeos.companion.ble.ProvisionState
import com.routeos.companion.model.DeviceStatus
import com.routeos.companion.model.ProvisionConfig
import kotlinx.coroutines.flow.StateFlow

class ProvisionViewModel(
    private val provisioner: BleProvisioner,
) : ViewModel() {

    val state: StateFlow<ProvisionState>    = provisioner.state
    val deviceStatus: StateFlow<DeviceStatus?> = provisioner.deviceStatus

    fun connect() = provisioner.connect()

    fun provision(config: ProvisionConfig) = provisioner.provision(config, viewModelScope)

    fun forceReport()  = provisioner.sendCommand(BleConstants.CMD_FORCE_REPORT,     viewModelScope)
    fun enableAp()     = provisioner.sendCommand(BleConstants.CMD_ENABLE_AP,        viewModelScope)
    fun factoryReset() = provisioner.sendCommand(BleConstants.CMD_FACTORY_RESET,    viewModelScope)

    override fun onCleared() {
        provisioner.disconnect()
        super.onCleared()
    }
}
