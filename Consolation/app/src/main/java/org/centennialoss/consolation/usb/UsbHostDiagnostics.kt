package org.centennialoss.consolation.usb

import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbManager
import org.centennialoss.consolation.core.capture.CaptureDevice
import org.centennialoss.consolation.logging.AppLog as Log

/**
 * Emits a paste-friendly USB host snapshot to Logcat for debugging UVC / hub / descriptor issues.
 *
 * Capture with:
 * `adb logcat -s ConsolationUsbDiag:I`
 *
 * Copy everything between `BEGIN_CONSOLATION_USB_DIAG` and `END_CONSOLATION_USB_DIAG`.
 */
object UsbHostDiagnostics {
    const val LOG_TAG: String = "ConsolationUsbDiag"

    private const val maxHexBytes: Int = 512
    private const val maxLogChunkChars: Int = 3500

    fun logFullScan(
        usbManager: UsbManager,
        reason: String,
        focus: CaptureDevice?,
        deviceKey: (UsbDevice) -> String,
    ) {
        val report = buildReport(usbManager, reason, focus, deviceKey)
        emitChunked(report)
    }

    private fun emitChunked(text: String) {
        val lines = text.lines()
        val chunk = StringBuilder()
        fun flush() {
            if (chunk.isNotEmpty()) {
                Log.i(LOG_TAG, chunk.toString())
                chunk.clear()
            }
        }
        for (line in lines) {
            val addLen = line.length + if (chunk.isEmpty()) 0 else 1
            if (chunk.length + addLen > maxLogChunkChars) {
                flush()
            }
            if (chunk.isNotEmpty()) {
                chunk.append('\n')
            }
            chunk.append(line)
        }
        flush()
    }

    private fun buildReport(
        usbManager: UsbManager,
        reason: String,
        focus: CaptureDevice?,
        deviceKey: (UsbDevice) -> String,
    ): String = buildString {
        appendLine("BEGIN_CONSOLATION_USB_DIAG")
        appendLine("adb_filter: adb logcat -s $LOG_TAG:I")
        appendLine("reason=$reason")
        appendLine("focus_capture_device_id=${focus?.id}")
        appendLine("attached_count=${usbManager.deviceList.size}")
        val devices = usbManager.deviceList.values.sortedBy { it.deviceName }
        for (device in devices) {
            appendLine("--- device ---")
            appendUsbDeviceSection(usbManager, device, focus, deviceKey)
        }
        appendLine("END_CONSOLATION_USB_DIAG")
    }

    private fun StringBuilder.appendUsbDeviceSection(
        usbManager: UsbManager,
        device: UsbDevice,
        focus: CaptureDevice?,
        deviceKey: (UsbDevice) -> String,
    ) {
        val key = deviceKey(device)
        appendLine("device_key=$key")
        appendLine("device_name=${device.deviceName}")
        appendLine("vid=0x%04x pid=0x%04x".format(device.vendorId, device.productId))
        appendLine("class=${device.deviceClass} subclass=${device.deviceSubclass} protocol=${device.deviceProtocol}")
        appendLine("product_name=${device.productName}")
        appendLine("manufacturer_name=${device.manufacturerName}")
        appendLine("version=${device.version}")
        appendLine("has_permission=${usbManager.hasPermission(device)}")
        appendLine("is_focus=${focus?.id == key}")
        val configCount = device.configurationCount
        appendLine("configuration_count=$configCount")
        for (configIndex in 0 until configCount) {
            val configuration = device.getConfiguration(configIndex)
            appendLine(
                "  cfg[$configIndex] id=${configuration.id} " +
                    "name=${configuration.name ?: ""} " +
                    "max_power_ma=${configuration.maxPower} " +
                    "self_powered=${configuration.isSelfPowered} " +
                    "interfaces=${configuration.interfaceCount}",
            )
            for (interfaceIndex in 0 until configuration.interfaceCount) {
                val intf = configuration.getInterface(interfaceIndex)
                appendLine(
                    "    if[$interfaceIndex] id=${intf.id} alt=${intf.alternateSetting} " +
                        "class=${intf.interfaceClass} sub=${intf.interfaceSubclass} " +
                        "proto=${intf.interfaceProtocol} endpoints=${intf.endpointCount}",
                )
            }
        }
        if (focus?.id == key && usbManager.hasPermission(device)) {
            appendLine("  --- raw_descriptors (UsbDeviceConnection) ---")
            val connection = usbManager.openDevice(device)
            if (connection == null) {
                appendLine("  raw_descriptors_open_failed")
            } else {
                try {
                    val raw = connection.rawDescriptors
                    appendLine("  raw_length=${raw.size}")
                    appendHexLines(raw, maxHexBytes)
                } finally {
                    connection.close()
                }
            }
        }
    }

    private fun StringBuilder.appendHexLines(raw: ByteArray, cap: Int) {
        val limit = minOf(raw.size, cap)
        var offset = 0
        while (offset < limit) {
            val end = minOf(offset + 32, limit)
            val slice = raw.copyOfRange(offset, end)
            val hex = slice.joinToString(" ") { b -> "%02x".format(b.toInt() and 0xff) }
            appendLine("  ${"%04x".format(offset)}: $hex")
            offset = end
        }
        if (raw.size > cap) {
            appendLine("  ... hex truncated (showing $cap of ${raw.size} bytes)")
        }
        if (raw.size >= 18) {
            val bcdLo = raw[2].toInt() and 0xff
            val bcdHi = raw[3].toInt() and 0xff
            val devClass = raw[4].toInt() and 0xff
            val devSub = raw[5].toInt() and 0xff
            val devProto = raw[6].toInt() and 0xff
            val vid = ((raw[9].toInt() and 0xff) shl 8) or (raw[8].toInt() and 0xff)
            val pid = ((raw[11].toInt() and 0xff) shl 8) or (raw[10].toInt() and 0xff)
            val numConfigs = raw[17].toInt() and 0xff
            appendLine(
                "  parsed_device_header: bcd_usb=$bcdHi.$bcdLo " +
                    "dev_class=$devClass dev_sub=$devSub dev_proto=$devProto " +
                    "vid=0x%04x pid=0x%04x num_configs=$numConfigs".format(vid, pid),
            )
        }
    }
}
