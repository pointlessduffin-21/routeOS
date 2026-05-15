package com.routeos.companion.ui.screens

import androidx.compose.foundation.background
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.routeos.companion.ui.theme.RouteOSCard
import com.routeos.companion.ui.theme.RouteOSGreen
import com.routeos.companion.ui.theme.RouteOSMuted
import com.routeos.companion.usb.UsbState
import com.routeos.companion.viewmodel.UsbSerialViewModel

private val BAUD_RATES = listOf(9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600)

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun UsbSerialScreen(viewModel: UsbSerialViewModel) {
    val state         by viewModel.state.collectAsState()
    val log           by viewModel.log.collectAsState()
    val connectedDev  by viewModel.connectedDevice.collectAsState()

    var command    by remember { mutableStateOf("") }
    var baudRate   by remember { mutableStateOf(115200) }
    var baudExpanded by remember { mutableStateOf(false) }

    val listState = rememberLazyListState()

    // Auto-scroll to bottom when new lines arrive
    LaunchedEffect(log.size) {
        if (log.isNotEmpty()) listState.animateScrollToItem(log.size - 1)
    }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .padding(16.dp),
    ) {
        // ─── Header ──────────────────────────────────────────────────────────
        Row(verticalAlignment = Alignment.CenterVertically) {
            Icon(
                imageVector = Icons.Default.Cable,
                contentDescription = null,
                tint = RouteOSGreen,
                modifier = Modifier.size(28.dp),
            )
            Spacer(Modifier.width(10.dp))
            Column {
                Text("USB Serial", fontWeight = FontWeight.Bold, fontSize = 20.sp)
                Text(
                    if (state == UsbState.CONNECTED) connectedDev ?: "Connected"
                    else state.name.lowercase().replaceFirstChar { it.uppercase() },
                    fontSize = 12.sp,
                    color = when (state) {
                        UsbState.CONNECTED -> RouteOSGreen
                        UsbState.ERROR     -> MaterialTheme.colorScheme.error
                        else               -> RouteOSMuted
                    },
                )
            }
        }

        Spacer(Modifier.height(16.dp))

        // ─── Controls ────────────────────────────────────────────────────────
        Row(
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            // Baud rate selector
            ExposedDropdownMenuBox(
                expanded = baudExpanded,
                onExpandedChange = { baudExpanded = it },
                modifier = Modifier.width(140.dp),
            ) {
                OutlinedTextField(
                    value = "$baudRate",
                    onValueChange = {},
                    readOnly = true,
                    label = { Text("Baud", fontSize = 11.sp) },
                    trailingIcon = { ExposedDropdownMenuDefaults.TrailingIcon(expanded = baudExpanded) },
                    modifier = Modifier.menuAnchor().fillMaxWidth(),
                    textStyle = LocalTextStyle.current.copy(fontSize = 13.sp),
                    singleLine = true,
                )
                ExposedDropdownMenu(expanded = baudExpanded, onDismissRequest = { baudExpanded = false }) {
                    BAUD_RATES.forEach { baud ->
                        DropdownMenuItem(
                            text = { Text("$baud") },
                            onClick = { baudRate = baud; baudExpanded = false },
                        )
                    }
                }
            }

            when (state) {
                UsbState.DISCONNECTED, UsbState.ERROR -> Button(
                    onClick = { viewModel.connect(baudRate) },
                    modifier = Modifier.weight(1f),
                    shape = RoundedCornerShape(10.dp),
                ) {
                    Icon(Icons.Default.Link, contentDescription = null, modifier = Modifier.size(18.dp))
                    Spacer(Modifier.width(6.dp))
                    Text("Connect")
                }
                UsbState.PERMISSION_PENDING, UsbState.CONNECTING -> {
                    Box(
                        modifier = Modifier.weight(1f),
                        contentAlignment = Alignment.Center,
                    ) {
                        CircularProgressIndicator(modifier = Modifier.size(24.dp), color = RouteOSGreen)
                    }
                }
                UsbState.CONNECTED -> Button(
                    onClick = { viewModel.disconnect() },
                    modifier = Modifier.weight(1f),
                    shape = RoundedCornerShape(10.dp),
                    colors = ButtonDefaults.buttonColors(containerColor = MaterialTheme.colorScheme.error),
                ) {
                    Icon(Icons.Default.LinkOff, contentDescription = null, modifier = Modifier.size(18.dp))
                    Spacer(Modifier.width(6.dp))
                    Text("Disconnect")
                }
            }

            // Clear button
            IconButton(onClick = { viewModel.clearLog() }) {
                Icon(Icons.Default.DeleteSweep, contentDescription = "Clear", tint = RouteOSMuted)
            }
        }

        Spacer(Modifier.height(12.dp))

        // ─── Log window ──────────────────────────────────────────────────────
        Box(
            modifier = Modifier
                .weight(1f)
                .fillMaxWidth()
                .clip(RoundedCornerShape(12.dp))
                .background(Color(0xFF0D1117)),
        ) {
            if (log.isEmpty()) {
                Text(
                    "Serial output will appear here…",
                    color = RouteOSMuted,
                    fontSize = 12.sp,
                    modifier = Modifier.align(Alignment.Center),
                )
            } else {
                LazyColumn(
                    state = listState,
                    modifier = Modifier
                        .fillMaxSize()
                        .horizontalScroll(rememberScrollState())
                        .padding(10.dp),
                    verticalArrangement = Arrangement.spacedBy(2.dp),
                ) {
                    items(log) { line ->
                        Text(
                            text = line,
                            color = logLineColor(line),
                            fontSize = 11.sp,
                            fontFamily = FontFamily.Monospace,
                            softWrap = false,
                        )
                    }
                }
            }
        }

        Spacer(Modifier.height(10.dp))

        // ─── Command input ────────────────────────────────────────────────────
        Row(
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            OutlinedTextField(
                value = command,
                onValueChange = { command = it },
                placeholder = { Text("Send command…", fontSize = 12.sp) },
                modifier = Modifier.weight(1f),
                singleLine = true,
                enabled = state == UsbState.CONNECTED,
                keyboardOptions = KeyboardOptions(imeAction = ImeAction.Send),
                keyboardActions = KeyboardActions(onSend = {
                    if (command.isNotBlank()) { viewModel.send(command); command = "" }
                }),
            )
            IconButton(
                onClick = { if (command.isNotBlank()) { viewModel.send(command); command = "" } },
                enabled = state == UsbState.CONNECTED && command.isNotBlank(),
            ) {
                Icon(Icons.Default.Send, contentDescription = "Send", tint = RouteOSGreen)
            }
        }
    }
}

private fun logLineColor(line: String): Color = when {
    line.startsWith("✅") || line.startsWith("[INFO]")  -> Color(0xFF7EE787)
    line.startsWith("❌") || line.startsWith("[ERROR]") -> Color(0xFFF85149)
    line.startsWith("⚠️")  || line.startsWith("[WARN]")  -> Color(0xFFE3B341)
    line.startsWith("→")                                -> Color(0xFF58A6FF)
    line.startsWith("🔍") || line.startsWith("🔒")      -> Color(0xFFA371F7)
    line.startsWith("🔌")                               -> Color(0xFF8B949E)
    else                                                -> Color(0xFFCDD9E5)
}
