package com.routeos.companion.ui.screens

import androidx.compose.animation.*
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Bluetooth
import androidx.compose.material.icons.filled.Search
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.routeos.companion.model.ScannedDevice
import com.routeos.companion.ui.theme.RouteOSCard
import com.routeos.companion.ui.theme.RouteOSGreen
import com.routeos.companion.ui.theme.RouteOSMuted
import com.routeos.companion.viewmodel.ScanViewModel

@Composable
fun ScanScreen(
    viewModel: ScanViewModel,
    onDeviceSelected: (ScannedDevice) -> Unit,
) {
    val devices  by viewModel.scannedDevices.collectAsState()
    val scanning by viewModel.isScanning.collectAsState()

    Column(
        modifier = Modifier
            .fillMaxSize()
            .padding(16.dp),
    ) {
        // ─── Header ──────────────────────────────────────────────────────────
        Row(verticalAlignment = Alignment.CenterVertically) {
            Icon(
                imageVector = Icons.Default.Bluetooth,
                contentDescription = null,
                tint = RouteOSGreen,
                modifier = Modifier.size(28.dp),
            )
            Spacer(Modifier.width(10.dp))
            Column {
                Text("routeOS Companion", fontWeight = FontWeight.Bold, fontSize = 20.sp)
                Text("Find & provision ECO-BIN devices", fontSize = 12.sp, color = RouteOSMuted)
            }
        }

        Spacer(Modifier.height(24.dp))

        // ─── Scan button ──────────────────────────────────────────────────────
        Button(
            onClick = { if (scanning) viewModel.stopScan() else viewModel.startScan() },
            modifier = Modifier.fillMaxWidth(),
            shape = RoundedCornerShape(12.dp),
        ) {
            Icon(
                imageVector = if (scanning) Icons.Default.Bluetooth else Icons.Default.Search,
                contentDescription = null,
            )
            Spacer(Modifier.width(8.dp))
            Text(if (scanning) "Stop Scan" else "Scan for Devices")
        }

        if (scanning) {
            Spacer(Modifier.height(12.dp))
            LinearProgressIndicator(modifier = Modifier.fillMaxWidth(), color = RouteOSGreen)
        }

        Spacer(Modifier.height(20.dp))

        // ─── Device list ─────────────────────────────────────────────────────
        if (devices.isEmpty() && !scanning) {
            Box(
                Modifier
                    .fillMaxWidth()
                    .padding(vertical = 48.dp),
                contentAlignment = Alignment.Center,
            ) {
                Column(horizontalAlignment = Alignment.CenterHorizontally) {
                    Text("🔍", fontSize = 40.sp)
                    Spacer(Modifier.height(8.dp))
                    Text("No ECO-BIN devices found", color = RouteOSMuted)
                    Text("Tap Scan to search", fontSize = 12.sp, color = RouteOSMuted)
                }
            }
        } else {
            Text(
                "${devices.size} device${if (devices.size != 1) "s" else ""} found",
                fontSize = 12.sp,
                color = RouteOSMuted,
            )
            Spacer(Modifier.height(8.dp))
            LazyColumn(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                items(devices, key = { it.address }) { device ->
                    DeviceListItem(device = device, onClick = { onDeviceSelected(device) })
                }
            }
        }
    }
}

@Composable
private fun DeviceListItem(device: ScannedDevice, onClick: () -> Unit) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(12.dp))
            .background(RouteOSCard)
            .clickable(onClick = onClick)
            .padding(horizontal = 16.dp, vertical = 14.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        // Signal strength bars
        RssiBars(rssi = device.rssi)
        Spacer(Modifier.width(14.dp))
        Column(Modifier.weight(1f)) {
            Text(device.name, fontWeight = FontWeight.SemiBold, fontSize = 16.sp)
            Text(device.address, fontSize = 11.sp, color = RouteOSMuted)
        }
        Text("${device.rssi} dBm", fontSize = 11.sp, color = RouteOSMuted)
    }
}

@Composable
private fun RssiBars(rssi: Int) {
    val bars = when {
        rssi >= -60 -> 4
        rssi >= -70 -> 3
        rssi >= -80 -> 2
        else        -> 1
    }
    Row(
        verticalAlignment = Alignment.Bottom,
        modifier = Modifier.width(20.dp).height(18.dp),
    ) {
        for (i in 1..4) {
            Box(
                Modifier
                    .width(3.dp)
                    .height((i * 4).dp)
                    .background(if (i <= bars) RouteOSGreen else RouteOSMuted)
            )
            if (i < 4) Spacer(Modifier.width(1.dp))
        }
    }
}
