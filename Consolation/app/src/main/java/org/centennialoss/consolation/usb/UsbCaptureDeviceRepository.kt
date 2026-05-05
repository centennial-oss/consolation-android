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
                CaptureDevice(
                    id = usbDevice.deviceKey(),
                    name = usbDevice.productName ?: usbDevice.deviceName,
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
