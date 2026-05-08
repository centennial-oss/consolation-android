package org.centennialoss.consolation.usb

import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.hardware.usb.UsbConstants
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbManager
import android.os.Build
import android.os.Parcelable
import androidx.core.content.ContextCompat
import org.centennialoss.consolation.core.capture.CaptureDevice
import org.centennialoss.consolation.logging.AppLog as Log
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import java.io.File

class UsbCaptureDeviceRepository(
    private val context: Context,
) {
    companion object {
        const val permissionAction: String = "org.centennialoss.consolation.USB_PERMISSION"
        private const val logTag: String = "UsbPermissionFlow"
    }

    private val usbManager = context.getSystemService(Context.USB_SERVICE) as UsbManager
    private val mutableDevices = MutableStateFlow<List<CaptureDevice>>(emptyList())
    val devices: StateFlow<List<CaptureDevice>> = mutableDevices.asStateFlow()

    private val mutablePermissionGranted = MutableStateFlow(false)
    val permissionGranted: StateFlow<Boolean> = mutablePermissionGranted.asStateFlow()
    private val mutablePermissionResults = MutableSharedFlow<PermissionResult>(extraBufferCapacity = 1)
    val permissionResults: SharedFlow<PermissionResult> = mutablePermissionResults
    private var pendingPermissionDeviceId: String? = null

    private val permissionIntent: PendingIntent by lazy {
        val baseFlags = PendingIntent.FLAG_UPDATE_CURRENT
        val mutableFlag = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            PendingIntent.FLAG_MUTABLE
        } else {
            0
        }
        PendingIntent.getBroadcast(
            context,
            0,
            Intent(permissionAction).setPackage(context.packageName),
            baseFlags or mutableFlag,
        )
    }

    private var isRegistered = false
    private val usbReceiver = object : BroadcastReceiver() {
        override fun onReceive(receiverContext: Context, intent: Intent) {
            when (intent.action) {
                UsbManager.ACTION_USB_DEVICE_ATTACHED,
                UsbManager.ACTION_USB_DEVICE_DETACHED -> refreshDevices()
                permissionAction -> {
                    val device = intent.parcelableExtraCompat<UsbDevice>(UsbManager.EXTRA_DEVICE)
                    val pendingDeviceId = pendingPermissionDeviceId
                    if (pendingDeviceId == null) {
                        Log.d(logTag, "Ignoring permission result: no pending device request.")
                        return
                    }
                    if (device == null || device.deviceKey() != pendingDeviceId) {
                        Log.d(
                            logTag,
                            "Ignoring permission result: device mismatch, " +
                                "pending=$pendingDeviceId received=${device?.deviceKey()}",
                        )
                        return
                    }
                    val hasGrantedExtra = intent.hasExtra(UsbManager.EXTRA_PERMISSION_GRANTED)
                    val grantedExtra = intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)
                    val hasPermissionNow = usbManager.hasPermission(device)
                    val granted = grantedExtra || hasPermissionNow
                    Log.d(
                        logTag,
                        "Permission result for ${device.deviceKey()}: " +
                            "extraPresent=$hasGrantedExtra extraGranted=$grantedExtra " +
                            "managerHasPermission=$hasPermissionNow resolvedGranted=$granted",
                    )
                    pendingPermissionDeviceId = null
                    mutablePermissionGranted.value = granted
                    if (granted) {
                        mutablePermissionResults.tryEmit(PermissionResult.Granted)
                    } else {
                        mutablePermissionResults.tryEmit(PermissionResult.Denied)
                    }
                    if (!granted) {
                        refreshDevices()
                    }
                }
            }
        }
    }

    fun start() {
        if (!isRegistered) {
            val filter = IntentFilter().apply {
                addAction(UsbManager.ACTION_USB_DEVICE_ATTACHED)
                addAction(UsbManager.ACTION_USB_DEVICE_DETACHED)
                addAction(permissionAction)
            }
            ContextCompat.registerReceiver(
                context,
                usbReceiver,
                filter,
                ContextCompat.RECEIVER_NOT_EXPORTED,
            )
            isRegistered = true
        }
        refreshDevices()
    }

    fun stop() {
        if (!isRegistered) {
            return
        }
        context.unregisterReceiver(usbReceiver)
        isRegistered = false
    }

    /**
     * Refresh the device list when the activity is (re)launched from a USB attach/detach intent.
     * Implicit permission from a manifest [usb_device_filter] is reflected after this runs.
     */
    fun refreshAfterUsbIntent(intent: Intent?) {
        val action = intent?.action ?: return
        if (action == UsbManager.ACTION_USB_DEVICE_ATTACHED ||
            action == UsbManager.ACTION_USB_DEVICE_DETACHED
        ) {
            refreshDevices()
        }
    }

    fun requestPermission(device: CaptureDevice): Boolean {
        val usbDevice = usbManager.deviceList.values.firstOrNull { it.deviceKey() == device.id }
            ?: run {
                Log.w(logTag, "Unable to request permission. Device not found for id=${device.id}")
                return false
            }
        pendingPermissionDeviceId = usbDevice.deviceKey()
        Log.d(logTag, "Requesting USB permission for ${usbDevice.deviceKey()}")
        usbManager.requestPermission(usbDevice, permissionIntent)
        return true
    }

    fun hasPermission(device: CaptureDevice): Boolean {
        val usbDevice = usbManager.deviceList.values.firstOrNull { it.deviceKey() == device.id }
            ?: return false
        return usbManager.hasPermission(usbDevice)
    }

    fun resolveUsbDevice(device: CaptureDevice): UsbDevice? {
        return usbManager.deviceList.values.firstOrNull { it.deviceKey() == device.id }
    }

    /**
     * Writes a structured USB snapshot to Logcat ([UsbHostDiagnostics.LOG_TAG]) for support
     * debugging (hub, UVC class, raw descriptors). Copy output between the BEGIN/END markers.
     */
    fun logUsbHostDiagnostics(reason: String, focus: CaptureDevice?) {
        UsbHostDiagnostics.logFullScan(
            usbManager,
            reason,
            focus,
            { usbDevice -> usbDevice.deviceKey() },
        )
    }

    private fun refreshDevices() {
        val captureDevices = usbManager.deviceList.values
            .filter { usbDevice -> usbDevice.hasUvcVideoControlInterface() }
            .sortedBy { it.deviceName }
            .map { usbDevice ->
                val name = usbDevice.productName ?: usbDevice.deviceName
                val speedLabel = usbDevice.usbSpeedLabel()
                CaptureDevice(
                    id = usbDevice.deviceKey(),
                    name = name,
                    displayName = if (speedLabel != null) "$name ($speedLabel)" else name,
                    vendorId = usbDevice.vendorId,
                    productId = usbDevice.productId,
                )
            }
        mutableDevices.value = captureDevices
        if (captureDevices.isEmpty()) {
            mutablePermissionGranted.value = false
        }
    }

    /**
     * USB-C hubs and controllers often enumerate a separate [UsbDevice] for the Billboard
     * (class 0x11) alongside the real UVC capture device. Only list devices that declare a
     * VideoControl interface so the picker cannot default to a non-camera node.
     */
    private fun UsbDevice.hasUvcVideoControlInterface(): Boolean {
        for (cfgIndex in 0 until configurationCount) {
            val configuration = getConfiguration(cfgIndex)
            for (interfaceIndex in 0 until configuration.interfaceCount) {
                val intf = configuration.getInterface(interfaceIndex)
                if (intf.interfaceClass == UsbConstants.USB_CLASS_VIDEO && intf.interfaceSubclass == 1) {
                    return true
                }
            }
        }
        return false
    }

    private fun UsbDevice.deviceKey(): String = "$vendorId:$productId:$deviceName"

    private fun UsbDevice.usbSpeedLabel(): String? {
        return actualUsbSpeedLabelFromSysfs()
            ?: descriptorUsbCapabilityLabel()
    }

    private fun UsbDevice.actualUsbSpeedLabelFromSysfs(): String? {
        val parts = deviceName.split('/')
        if (parts.size < 2) return null
        val busNumber = parts.getOrNull(parts.size - 2)?.toIntOrNull() ?: return null
        val deviceNumber = parts.lastOrNull()?.toIntOrNull() ?: return null
        val sysfsRoot = File("/sys/bus/usb/devices")
        val children = sysfsRoot.listFiles() ?: return null

        for (child in children) {
            val bus = child.resolve("busnum").readTextOrNull()?.trim()?.toIntOrNull()
            val dev = child.resolve("devnum").readTextOrNull()?.trim()?.toIntOrNull()
            if (bus == busNumber && dev == deviceNumber) {
                val mbps = child.resolve("speed").readTextOrNull()?.trim()?.toDoubleOrNull()
                    ?: return null
                return usbSpeedLabelFromMbps(mbps)
            }
        }
        return null
    }

    private fun UsbDevice.descriptorUsbCapabilityLabel(): String? {
        if (!usbManager.hasPermission(this)) return null
        val connection = try {
            usbManager.openDevice(this)
        } catch (_: Exception) {
            null
        } ?: return null

        return try {
            val raw = connection.rawDescriptors ?: return null
            if (raw.size < 4) return null
            val bcdUsb = raw.u8(2) or (raw.u8(3) shl 8)
            when {
                bcdUsb >= 0x0300 -> "USB 3+ capable"
                bcdUsb >= 0x0200 -> "USB 2 capable"
                else -> "USB 1 capable"
            }
        } catch (_: Exception) {
            null
        } finally {
            runCatching { connection.close() }
        }
    }

    private fun usbSpeedLabelFromMbps(mbps: Double): String {
        return when {
            mbps >= 10000.0 -> "USB 3+ ${formatGbps(mbps)}"
            mbps >= 5000.0 -> "USB 3 ${formatGbps(mbps)}"
            mbps >= 480.0 -> "USB 2 480 Mbps"
            mbps >= 12.0 -> "USB 1.1 12 Mbps"
            else -> "USB 1 ${mbps.toInt()} Mbps"
        }
    }

    private fun formatGbps(mbps: Double): String {
        val gbps = mbps / 1000.0
        return if (gbps % 1.0 == 0.0) {
            "${gbps.toInt()} Gbps"
        } else {
            "%.1f Gbps".format(gbps)
        }
    }

    private fun File.readTextOrNull(): String? {
        return try {
            if (isFile && canRead()) readText() else null
        } catch (_: Exception) {
            null
        }
    }

    private fun ByteArray.u8(index: Int): Int = this[index].toInt() and 0xff

    sealed interface PermissionResult {
        data object Granted : PermissionResult
        data object Denied : PermissionResult
    }
}

private inline fun <reified T : Parcelable> Intent.parcelableExtraCompat(name: String): T? {
    return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
        getParcelableExtra(name, T::class.java)
    } else {
        @Suppress("DEPRECATION")
        getParcelableExtra(name)
    }
}
