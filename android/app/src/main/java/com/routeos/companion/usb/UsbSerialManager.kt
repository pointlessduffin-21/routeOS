package com.routeos.companion.usb

import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbManager
import android.os.Build
import com.hoho.android.usbserial.driver.UsbSerialDriver
import com.hoho.android.usbserial.driver.UsbSerialPort
import com.hoho.android.usbserial.driver.UsbSerialProber
import kotlinx.coroutines.*
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow

private const val ACTION_USB_PERMISSION = "com.routeos.companion.USB_PERMISSION"
private const val READ_BUFFER_SIZE      = 4096
private const val READ_TIMEOUT_MS       = 200

enum class UsbState { DISCONNECTED, PERMISSION_PENDING, CONNECTING, CONNECTED, ERROR }

data class UsbSerialConfig(
    val baudRate: Int   = 115200,
    val dataBits: Int   = UsbSerialPort.DATABITS_8,
    val stopBits: Int   = UsbSerialPort.STOPBITS_1,
    val parity:   Int   = UsbSerialPort.PARITY_NONE,
)

class UsbSerialManager(private val context: Context) {

    private val usbManager = context.getSystemService(Context.USB_SERVICE) as UsbManager

    private val _state      = MutableStateFlow(UsbState.DISCONNECTED)
    val state: StateFlow<UsbState> = _state.asStateFlow()

    private val _log        = MutableStateFlow<List<String>>(emptyList())
    val log: StateFlow<List<String>> = _log.asStateFlow()

    private val _connectedDevice = MutableStateFlow<String?>(null)
    val connectedDevice: StateFlow<String?> = _connectedDevice.asStateFlow()

    private var port:     UsbSerialPort? = null
    private var readJob:  Job?           = null
    private val scope = CoroutineScope(Dispatchers.IO + SupervisorJob())

    // ─── USB permission broadcast receiver ───────────────────────────────────

    private val permissionReceiver = object : BroadcastReceiver() {
        override fun onReceive(ctx: Context, intent: Intent) {
            if (intent.action != ACTION_USB_PERMISSION) return
            val device = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU)
                intent.getParcelableExtra(UsbManager.EXTRA_DEVICE, UsbDevice::class.java)
            else
                @Suppress("DEPRECATION") intent.getParcelableExtra(UsbManager.EXTRA_DEVICE)

            if (intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)) {
                device?.let { openPort(it) }
            } else {
                appendLog("⛔ USB permission denied")
                _state.value = UsbState.DISCONNECTED
            }
        }
    }

    init {
        val filter = IntentFilter(ACTION_USB_PERMISSION)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU)
            context.registerReceiver(permissionReceiver, filter, Context.RECEIVER_NOT_EXPORTED)
        else
            context.registerReceiver(permissionReceiver, filter)
    }

    // ─── Public API ──────────────────────────────────────────────────────────

    /** Returns all connected serial-capable USB devices. */
    fun findDevices(): List<UsbSerialDriver> =
        UsbSerialProber.getDefaultProber().findAllDrivers(usbManager)

    /** Connect to the first detected ESP32/serial device with optional config. */
    fun connect(config: UsbSerialConfig = UsbSerialConfig()) {
        val drivers = findDevices()
        if (drivers.isEmpty()) {
            appendLog("🔌 No USB serial device found — is the OTG cable connected?")
            _state.value = UsbState.ERROR
            return
        }
        val driver = drivers.first()
        val device = driver.device
        appendLog("🔍 Found: ${device.productName ?: device.deviceName} (${String.format("%04X:%04X", device.vendorId, device.productId)})")

        if (!usbManager.hasPermission(device)) {
            _state.value = UsbState.PERMISSION_PENDING
            val flags = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S)
                PendingIntent.FLAG_MUTABLE
            else 0
            val permIntent = PendingIntent.getBroadcast(context, 0, Intent(ACTION_USB_PERMISSION), flags)
            usbManager.requestPermission(device, permIntent)
            appendLog("🔒 Requesting USB permission…")
        } else {
            openPort(device, config)
        }
    }

    /** Disconnect and release the port. */
    fun disconnect() {
        readJob?.cancel()
        try { port?.close() } catch (_: Exception) {}
        port = null
        _state.value = UsbState.DISCONNECTED
        _connectedDevice.value = null
        appendLog("🔌 Disconnected")
    }

    /** Write a string line to the ESP32 serial port. */
    fun send(text: String) {
        val p = port ?: run { appendLog("❌ Not connected"); return }
        scope.launch {
            try {
                p.write((text + "\n").toByteArray(), 2000)
                appendLog("→ $text")
            } catch (e: Exception) {
                appendLog("❌ Write error: ${e.message}")
            }
        }
    }

    fun clearLog() { _log.value = emptyList() }

    fun release() {
        disconnect()
        try { context.unregisterReceiver(permissionReceiver) } catch (_: Exception) {}
        scope.cancel()
    }

    // ─── Internal ────────────────────────────────────────────────────────────

    private fun openPort(device: UsbDevice, config: UsbSerialConfig = UsbSerialConfig()) {
        val drivers = UsbSerialProber.getDefaultProber().findAllDrivers(usbManager)
        val driver  = drivers.firstOrNull { it.device == device } ?: return
        val conn    = usbManager.openDevice(device) ?: run {
            appendLog("❌ Failed to open USB device")
            _state.value = UsbState.ERROR
            return
        }

        _state.value = UsbState.CONNECTING
        val p = driver.ports[0]
        try {
            p.open(conn)
            p.setParameters(config.baudRate, config.dataBits, config.stopBits, config.parity)
        } catch (e: Exception) {
            appendLog("❌ Port open error: ${e.message}")
            _state.value = UsbState.ERROR
            return
        }

        port = p
        _state.value = UsbState.CONNECTED
        _connectedDevice.value = "${device.productName ?: "ESP32"} @ ${config.baudRate} baud"
        appendLog("✅ Connected: ${_connectedDevice.value}")

        startReading()
    }

    private fun startReading() {
        readJob?.cancel()
        readJob = scope.launch {
            val buf = ByteArray(READ_BUFFER_SIZE)
            val sb  = StringBuilder()
            while (isActive) {
                val p = port ?: break
                try {
                    val n = p.read(buf, READ_TIMEOUT_MS)
                    if (n > 0) {
                        sb.append(String(buf, 0, n, Charsets.UTF_8))
                        // Emit complete lines
                        while (sb.contains('\n')) {
                            val nl  = sb.indexOf('\n')
                            val line = sb.substring(0, nl).trimEnd('\r')
                            sb.delete(0, nl + 1)
                            if (line.isNotBlank()) appendLog(line)
                        }
                    }
                } catch (e: Exception) {
                    if (isActive) {
                        appendLog("⚠️ Read error: ${e.message}")
                        _state.value = UsbState.ERROR
                    }
                    break
                }
            }
        }
    }

    private fun appendLog(line: String) {
        _log.value = (_log.value + line).takeLast(500)
    }
}
