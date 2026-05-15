package com.routeos.companion.ui.screens

import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.text.input.VisualTransformation
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.routeos.companion.ble.ProvisionState
import com.routeos.companion.model.ProvisionConfig
import com.routeos.companion.model.ScannedDevice
import com.routeos.companion.ui.theme.RouteOSGreen
import com.routeos.companion.ui.theme.RouteOSMuted
import com.routeos.companion.ui.theme.RouteOSOrange
import com.routeos.companion.viewmodel.ProvisionViewModel

@Composable
fun ProvisionScreen(
    device: ScannedDevice,
    viewModel: ProvisionViewModel,
    onBack: () -> Unit,
) {
    val state        by viewModel.state.collectAsState()
    val deviceStatus by viewModel.deviceStatus.collectAsState()

    // Form state
    var wifiSsid       by remember { mutableStateOf("") }
    var wifiPass       by remember { mutableStateOf("") }
    var showPass       by remember { mutableStateOf(false) }
    var deviceCode     by remember { mutableStateOf(device.name) }
    var apiUrl         by remember { mutableStateOf("http://192.168.1.100:4003/api/devices") }
    var intervalSec    by remember { mutableStateOf("900") }
    var binHeightCm    by remember { mutableStateOf("100") }

    // Auto-connect on first composition
    LaunchedEffect(Unit) { viewModel.connect() }

    // Pre-fill device code from BLE status notification
    LaunchedEffect(deviceStatus) {
        deviceStatus?.let {
            if (it.deviceCode.isNotBlank() && deviceCode == device.name) deviceCode = it.deviceCode
            if (it.wifiSsid.isNotBlank() && wifiSsid.isEmpty()) wifiSsid = it.wifiSsid
        }
    }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(16.dp),
    ) {
        // ─── Top bar ─────────────────────────────────────────────────────────
        Row(verticalAlignment = Alignment.CenterVertically) {
            IconButton(onClick = onBack) {
                Icon(Icons.Default.ArrowBack, contentDescription = "Back")
            }
            Column {
                Text(device.name, fontWeight = FontWeight.Bold, fontSize = 18.sp)
                Text(device.address, fontSize = 11.sp, color = RouteOSMuted)
            }
        }

        Spacer(Modifier.height(16.dp))

        // ─── Connection + live status card ───────────────────────────────────
        ConnectionStatusCard(state = state, deviceStatus = deviceStatus)

        Spacer(Modifier.height(20.dp))

        // ─── Form ─────────────────────────────────────────────────────────────
        Text("WiFi Network", fontWeight = FontWeight.SemiBold, color = RouteOSGreen)
        Spacer(Modifier.height(8.dp))

        OutlinedTextField(
            value = wifiSsid,
            onValueChange = { wifiSsid = it },
            label = { Text("SSID") },
            leadingIcon = { Icon(Icons.Default.Wifi, null) },
            modifier = Modifier.fillMaxWidth(),
            singleLine = true,
        )
        Spacer(Modifier.height(8.dp))
        OutlinedTextField(
            value = wifiPass,
            onValueChange = { wifiPass = it },
            label = { Text("Password") },
            leadingIcon = { Icon(Icons.Default.Lock, null) },
            trailingIcon = {
                IconButton(onClick = { showPass = !showPass }) {
                    Icon(if (showPass) Icons.Default.VisibilityOff else Icons.Default.Visibility, null)
                }
            },
            visualTransformation = if (showPass) VisualTransformation.None else PasswordVisualTransformation(),
            modifier = Modifier.fillMaxWidth(),
            singleLine = true,
        )

        Spacer(Modifier.height(20.dp))
        Text("Device Configuration", fontWeight = FontWeight.SemiBold, color = RouteOSGreen)
        Spacer(Modifier.height(8.dp))

        OutlinedTextField(
            value = deviceCode,
            onValueChange = { deviceCode = it },
            label = { Text("Device Code") },
            leadingIcon = { Icon(Icons.Default.Badge, null) },
            modifier = Modifier.fillMaxWidth(),
            singleLine = true,
        )
        Spacer(Modifier.height(8.dp))
        OutlinedTextField(
            value = apiUrl,
            onValueChange = { apiUrl = it },
            label = { Text("Backend API URL") },
            leadingIcon = { Icon(Icons.Default.Cloud, null) },
            modifier = Modifier.fillMaxWidth(),
            singleLine = true,
            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Uri),
        )
        Spacer(Modifier.height(8.dp))
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            OutlinedTextField(
                value = intervalSec,
                onValueChange = { intervalSec = it.filter(Char::isDigit) },
                label = { Text("Interval (sec)") },
                modifier = Modifier.weight(1f),
                singleLine = true,
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
            )
            OutlinedTextField(
                value = binHeightCm,
                onValueChange = { binHeightCm = it.filter { c -> c.isDigit() || c == '.' } },
                label = { Text("Bin Height (cm)") },
                modifier = Modifier.weight(1f),
                singleLine = true,
                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Decimal),
            )
        }

        Spacer(Modifier.height(24.dp))

        // ─── Provision button ─────────────────────────────────────────────────
        val busy = state is ProvisionState.Connecting || state is ProvisionState.Writing
        val done = state is ProvisionState.Done

        Button(
            onClick = {
                viewModel.provision(
                    ProvisionConfig(
                        wifiSsid       = wifiSsid,
                        wifiPass       = wifiPass,
                        deviceCode     = deviceCode,
                        apiUrl         = apiUrl,
                        reportIntervalSec = intervalSec.toIntOrNull() ?: 900,
                        binHeightCm    = binHeightCm.toFloatOrNull() ?: 100f,
                    )
                )
            },
            modifier = Modifier.fillMaxWidth(),
            enabled = !busy && !done && wifiSsid.isNotBlank(),
            shape = RoundedCornerShape(12.dp),
        ) {
            if (busy) {
                CircularProgressIndicator(
                    modifier = Modifier.size(18.dp),
                    strokeWidth = 2.dp,
                    color = MaterialTheme.colorScheme.onPrimary,
                )
                Spacer(Modifier.width(8.dp))
                val step = (state as? ProvisionState.Writing)?.step ?: "Connecting…"
                Text(step)
            } else {
                Icon(Icons.Default.FlashOn, null)
                Spacer(Modifier.width(8.dp))
                Text("Provision Device")
            }
        }

        // ─── Advanced commands ────────────────────────────────────────────────
        if (state is ProvisionState.Connected || state is ProvisionState.Done) {
            Spacer(Modifier.height(20.dp))
            HorizontalDivider()
            Spacer(Modifier.height(12.dp))
            Text("Advanced", fontSize = 12.sp, color = RouteOSMuted)
            Spacer(Modifier.height(8.dp))
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                OutlinedButton(onClick = { viewModel.forceReport() }, modifier = Modifier.weight(1f)) {
                    Text("Force Report", fontSize = 12.sp)
                }
                OutlinedButton(onClick = { viewModel.enableAp() }, modifier = Modifier.weight(1f)) {
                    Text("Enable AP", fontSize = 12.sp)
                }
            }
            Spacer(Modifier.height(8.dp))
            OutlinedButton(
                onClick = { viewModel.factoryReset() },
                modifier = Modifier.fillMaxWidth(),
                colors = ButtonDefaults.outlinedButtonColors(contentColor = RouteOSOrange),
            ) {
                Icon(Icons.Default.DeleteForever, null)
                Spacer(Modifier.width(6.dp))
                Text("Factory Reset")
            }
        }

        Spacer(Modifier.height(32.dp))
    }
}

@Composable
private fun ConnectionStatusCard(
    state: ProvisionState,
    deviceStatus: com.routeos.companion.model.DeviceStatus?,
) {
    val (icon, label, color) = when (state) {
        is ProvisionState.Idle       -> Triple("○", "Idle", RouteOSMuted)
        is ProvisionState.Connecting -> Triple("◌", "Connecting…", RouteOSGreen)
        is ProvisionState.Connected  -> Triple("●", "Connected", RouteOSGreen)
        is ProvisionState.Writing    -> Triple("↑", state.step, RouteOSGreen)
        is ProvisionState.Done       -> Triple("✓", state.message, RouteOSGreen)
        is ProvisionState.Error      -> Triple("✕", state.message, RouteOSOrange)
    }

    Card(
        modifier = Modifier.fillMaxWidth(),
        shape = RoundedCornerShape(12.dp),
        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surfaceVariant),
    ) {
        Column(Modifier.padding(16.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(icon, fontSize = 18.sp, color = color)
                Spacer(Modifier.width(10.dp))
                Text(label, fontWeight = FontWeight.Medium, color = color)
            }
            deviceStatus?.let { st ->
                Spacer(Modifier.height(10.dp))
                HorizontalDivider(color = RouteOSMuted.copy(alpha = 0.3f))
                Spacer(Modifier.height(10.dp))
                Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                    StatusChip("🔋 ${st.batteryPercent}%")
                    StatusChip(if (st.wifiConnected) "📶 ${st.wifiSsid}" else "📵 No WiFi")
                    StatusChip("fw ${st.firmwareVersion}")
                }
            }
        }
    }
}

@Composable
private fun StatusChip(text: String) {
    Text(text, fontSize = 11.sp, color = RouteOSMuted)
}
