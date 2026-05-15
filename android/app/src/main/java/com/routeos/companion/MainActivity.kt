package com.routeos.companion

import android.Manifest
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothManager
import android.content.Intent
import android.os.Build
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Bluetooth
import androidx.compose.material.icons.filled.Cable
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.lifecycle.ViewModelProvider
import com.routeos.companion.model.ScannedDevice
import com.routeos.companion.ui.screens.ProvisionScreen
import com.routeos.companion.ui.screens.ScanScreen
import com.routeos.companion.ui.screens.UsbSerialScreen
import com.routeos.companion.ui.theme.RouteOSTheme
import com.routeos.companion.viewmodel.ProvisionViewModel
import com.routeos.companion.viewmodel.ScanViewModel
import com.routeos.companion.viewmodel.UsbSerialViewModel

class MainActivity : ComponentActivity() {

    private lateinit var scanViewModel: ScanViewModel
    private lateinit var usbViewModel:  UsbSerialViewModel

    private val permissionLauncher = registerForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions()
    ) { /* handled reactively in UI */ }

    private val enableBleLauncher = registerForActivityResult(
        ActivityResultContracts.StartActivityForResult()
    ) { /* user enabled/denied BLE */ }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        scanViewModel = ViewModelProvider(this)[ScanViewModel::class.java]
        usbViewModel  = ViewModelProvider(this)[UsbSerialViewModel::class.java]

        requestBlePermissions()

        setContent {
            RouteOSTheme {
                AppNavigation(
                    scanViewModel = scanViewModel,
                    usbViewModel  = usbViewModel,
                )
            }
        }
    }

    private fun requestBlePermissions() {
        val permissions = buildList {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                add(Manifest.permission.BLUETOOTH_SCAN)
                add(Manifest.permission.BLUETOOTH_CONNECT)
            } else {
                add(Manifest.permission.BLUETOOTH)
                add(Manifest.permission.BLUETOOTH_ADMIN)
                add(Manifest.permission.ACCESS_FINE_LOCATION)
            }
            if (Build.VERSION.SDK_INT < Build.VERSION_CODES.S) {
                add(Manifest.permission.ACCESS_COARSE_LOCATION)
            }
        }
        permissionLauncher.launch(permissions.toTypedArray())

        val btManager = getSystemService(BLUETOOTH_SERVICE) as BluetoothManager
        if (!btManager.adapter.isEnabled) {
            enableBleLauncher.launch(Intent(BluetoothAdapter.ACTION_REQUEST_ENABLE))
        }
    }
}

private enum class NavTab { BLE, USB }

@Composable
private fun AppNavigation(
    scanViewModel: ScanViewModel,
    usbViewModel:  UsbSerialViewModel,
) {
    var selectedTab    by remember { mutableStateOf(NavTab.BLE) }
    var selectedDevice by remember { mutableStateOf<ScannedDevice?>(null) }
    var provisionViewModel by remember { mutableStateOf<ProvisionViewModel?>(null) }

    Scaffold(
        bottomBar = {
            // Only show bottom nav when not deep in the BLE provision flow
            if (selectedDevice == null) {
                NavigationBar {
                    NavigationBarItem(
                        selected = selectedTab == NavTab.BLE,
                        onClick  = { selectedTab = NavTab.BLE },
                        icon     = { Icon(Icons.Default.Bluetooth, contentDescription = null) },
                        label    = { Text("BLE Scan") },
                    )
                    NavigationBarItem(
                        selected = selectedTab == NavTab.USB,
                        onClick  = { selectedTab = NavTab.USB },
                        icon     = { Icon(Icons.Default.Cable, contentDescription = null) },
                        label    = { Text("USB Serial") },
                    )
                }
            }
        },
    ) { innerPadding ->
        when {
            selectedDevice != null -> {
                val vm = provisionViewModel
                if (vm != null) {
                    ProvisionScreen(
                        device    = selectedDevice!!,
                        viewModel = vm,
                        onBack    = {
                            vm.onCleared()
                            provisionViewModel = null
                            selectedDevice     = null
                        },
                    )
                }
            }
            selectedTab == NavTab.BLE -> ScanScreen(
                viewModel       = scanViewModel,
                onDeviceSelected = { device ->
                    val provisioner = scanViewModel.createProvisioner(device.address)
                    provisionViewModel = ProvisionViewModel(provisioner)
                    selectedDevice     = device
                },
            )
            selectedTab == NavTab.USB -> UsbSerialScreen(viewModel = usbViewModel)
        }
    }
}
