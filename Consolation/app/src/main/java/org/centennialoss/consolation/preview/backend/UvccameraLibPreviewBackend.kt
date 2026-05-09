package org.centennialoss.consolation.preview.backend

import android.content.Context
import android.graphics.SurfaceTexture
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbDeviceConnection
import android.hardware.usb.UsbManager
import android.media.MediaCodec
import android.media.MediaFormat
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.view.Surface
import android.view.TextureView
import android.widget.Toast
import org.centennialoss.consolation.uvc.Size
import org.centennialoss.consolation.uvc.UVCCamera
import org.centennialoss.consolation.uvc.USBMonitor
import org.centennialoss.consolation.R
import org.centennialoss.consolation.audio.UsbCaptureAudioLoop
import org.centennialoss.consolation.core.capture.CaptureDevice
import org.centennialoss.consolation.core.telemetry.TelemetrySnapshot
import org.centennialoss.consolation.logging.AppLog as Log
import org.centennialoss.consolation.preview.UsbPreviewSurfaceHints.applyForUsbPreviewLatency
import org.centennialoss.consolation.preview.UsbPreviewSurfaceHints.setDefaultBufferSizeIfValid
import org.centennialoss.consolation.usb.UsbCaptureDeviceRepository
import java.nio.ByteBuffer
import java.nio.charset.StandardCharsets
import java.util.Locale
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicReference

/**
 * Host-side UVC using vendored UVCCamera JNI built into the app (`org.centennialoss.consolation.uvc`).
 * USB permission must already be granted via [UsbCaptureDeviceRepository].
 */
class UvccameraLibPreviewBackend(
    context: Context,
    private val usbRepository: UsbCaptureDeviceRepository,
) : UsbVideoPreviewBackend {
    override val telemetryBackendLabel: String = "uvccamera-lib-jni"

    private val appContext = context.applicationContext
    private var preferredDevice: CaptureDevice? = null
    private var textureView: TextureView? = null

    private var usbMonitorRef: USBMonitor? = null

    private var targetWidth = -1
    private var targetHeight = -1
    private var targetFps = -1
    private var preferredPixelFormatOverride: Int? = null

    private var currentWidth = -1
    private var currentHeight = -1
    private var currentFpsConfigured = -1
    private var currentPixelFormat = ""

    private var rotationDegrees = 0
    private var flipHorizontal = false
    private var flipVertical = false

    private var captureAudioVolumeLinear = 1f

    private val frameCount = AtomicInteger(0)
    private var lastTelemetryTime = System.currentTimeMillis()
    private var actualFps = 0
    private var droppedFrames = 0

    /**
     * Device last passed to [USBMonitor.openDevice] for preview; kept so we can
     * [USBMonitor.releaseCachedDevice] when the cable is pulled even if [UsbManager] no longer
     * lists the device.
     */
    private var monitorCachedUsbDevice: UsbDevice? = null

    private fun ensureUsbMonitor(): USBMonitor {
        val existing = usbMonitorRef
        if (existing != null) {
            if (!existing.isRegistered) {
                runCatching { existing.register() }
                    .onFailure { Log.w(logTag, "USBMonitor.register failed", it) }
            }
            return existing
        }
        val created = USBMonitor(
            appContext,
            object : USBMonitor.OnDeviceConnectListener {
                override fun onAttach(device: UsbDevice) = Unit
                override fun onDettach(device: UsbDevice) = Unit
                override fun onConnect(
                    device: UsbDevice,
                    ctrlBlock: USBMonitor.UsbControlBlock,
                    createNew: Boolean,
                ) = Unit

                override fun onDisconnect(device: UsbDevice, ctrlBlock: USBMonitor.UsbControlBlock) =
                    Unit

                override fun onCancel(device: UsbDevice) = Unit
            },
        )
        runCatching { created.register() }
            .onFailure { Log.w(logTag, "USBMonitor.register failed", it) }
        usbMonitorRef = created
        return created
    }

    @Volatile
    private var uvcCamera: UVCCamera? = null
    @Volatile
    private var previewRunning: Boolean = false
    private val h264DecoderLock = Any()
    private var h264Decoder: MediaCodec? = null
    private var h264OutputSurface: Surface? = null
    private var h264FramePtsUs = 0L

    private val captureAudioRef = AtomicReference<UsbCaptureAudioLoop?>(null)
    private var lastPreviewStartFailed: Boolean = false

    /** Set when the last [probeSupportedSizes] ended empty after a native open failure (-99, busy, etc.). */
    private var lastProbeOpenFailed: Boolean = false

    private val mainHandler = Handler(Looper.getMainLooper())
    private var deferredAudioRunnable: Runnable? = null
    private var deferredAudioFallbackRunnable: Runnable? = null
    private var captureAudioFailureListener: (() -> Unit)? = null
    private var pendingAudioUsbDevice: UsbDevice? = null
    private var pendingAudioCamera: UVCCamera? = null
    private var pendingAudioGeneration: Long = 0L

    /**
     * Preview connect/startPreview must never block the main thread (Input ANR). SurfaceTexture is
     * captured on the main looper, then USB/native work runs here.
     * All UVC teardown must be serialized on this thread with preview start so IO-thread stops
     * cannot race open/destroy; see [stopUvcStreamingBlocking].
     */
    private var uvcPreviewExecutor: ExecutorService = newPreviewExecutor()

    private fun newPreviewExecutor(): ExecutorService =
        Executors.newSingleThreadExecutor { r ->
            Thread(r, UVC_PREVIEW_THREAD_NAME).apply { isDaemon = true }
        }

    /**
     * Incremented when tearing down preview so a delayed USB-audio start is ignored.
     * Also compare after [UsbCaptureAudioLoop.start] races with stop.
     */
    private var playbackGeneration: Long = 0L

    private val loggedFirstVideoFrame = AtomicBoolean(false)
    private var lastNativeEndToEndLatencyAvgMs: Double = 0.0
    private var lastNativeQueuedAvgFrames: Double = 0.0
    private var lastNativePayloadAvgKb: Double = 0.0
    private var lastNativePreviewConvAvgMs: Double = 0.0
    private var lastNativeEndToEndMaxMs: Double = 0.0
    private var lastNativeQueueDeqMaxFrames: Double = 0.0
    private var lastNativeQueueEnqAvgFrames: Double = 0.0
    private var lastNativeQueueEnqMaxFrames: Double = 0.0
    private var lastNativeUvcCbAvgMs: Double = 0.0
    private var lastNativeUvcCbMaxMs: Double = 0.0
    private var lastNativeCbLagAvgMs: Double = 0.0
    private var lastNativeCbLagMaxMs: Double = 0.0
    private var lastNativeCbLagCount: Long = 0L
    private var lastNativePubFps: Double = 0.0
    private var lastNativePreCbSkip: Long = 0L
    private var lastNativeStreamDrop: Long = 0L
    private var lastNativeFrameInterval100ns: Long = 0L
    private var lastNativeAltSetting: Long = 0L
    private var lastNativeIsIsochronous: Boolean = false
    private var lastNativePublishedCountRaw: Long = 0L

    private val surfaceListener = object : TextureView.SurfaceTextureListener {
        override fun onSurfaceTextureAvailable(surface: SurfaceTexture, width: Int, height: Int) {
            requestStartPreview(surface)
        }

        override fun onSurfaceTextureSizeChanged(surface: SurfaceTexture, width: Int, height: Int) =
            Unit

        override fun onSurfaceTextureDestroyed(surface: SurfaceTexture): Boolean {
            stopUvcStreamingAsync()
            return true
        }

        override fun onSurfaceTextureUpdated(surface: SurfaceTexture) = Unit
    }

    /**
     * Host-initiated USB reset (usbfs `IOCTL_USBFS_RESET`), analogous to `libusb_reset_device`.
     * Call only after UVC teardown and [USBMonitor.releaseCachedDevice] so no fd holds the device.
     *
     * @return 0 on success; negative errno-style values or sentinel negatives on failure.
     */
    private fun kernelUsbResetIfPossible(usbDevice: UsbDevice): Int {
        val usbMgr = appContext.getSystemService(Context.USB_SERVICE) as? UsbManager ?: return -1
        if (!usbMgr.hasPermission(usbDevice)) {
            Log.w(logTag, "kernelUsbResetIfPossible: no USB permission")
            return -2
        }
        var conn: UsbDeviceConnection? = null
        return try {
            conn = usbMgr.openDevice(usbDevice)
            if (conn == null) {
                Log.w(logTag, "kernelUsbResetIfPossible: openDevice returned null")
                -3
            } else {
                val rc = UVCCamera.ioctlUsbReset(conn)
                if (rc == 0) {
                    Log.i(logTag, "kernelUsbResetIfPossible: USB reset ok")
                } else {
                    Log.w(logTag, "kernelUsbResetIfPossible: ioctlUsbReset rc=$rc")
                }
                rc
            }
        } finally {
            runCatching { conn?.close() }
        }
    }

    override fun prepareForUsbRemoval() {
        monitorCachedUsbDevice?.let { dev ->
            runCatching { ensureUsbMonitor().releaseCachedDevice(dev) }
                .onFailure { Log.w(logTag, "prepareForUsbRemoval: releaseCachedDevice", it) }
        }
        abandonNativeState("usb removal during playback")
    }

    override fun configureSession(preferredDevice: CaptureDevice?) {
        lastPreviewStartFailed = false
        // Always tear down any native UVC instance before a new play so negotiation
        // (resolution/FPS) starts from a clean device state — avoids black preview when
        // the card was left on a different mode than the chosen format.
        stopUvcStreamingBlocking()
        this.preferredDevice = preferredDevice
        val usbDev = preferredDevice?.let { usbRepository.resolveUsbDevice(it) }
        if (usbDev != null) {
            // Drop cached UsbDeviceConnection so the next open isn't stuck on an alt-setting /
            // bandwidth state after switching frame rates (e.g. 1080p30 → 1080p60).
            ensureUsbMonitor().releaseCachedDevice(usbDev)
            try {
                Thread.sleep(POST_USB_RELEASE_SETTLE_MS)
            } catch (_: InterruptedException) {
            }
            val resetRc = kernelUsbResetIfPossible(usbDev)
            if (resetRc != 0) {
                Log.w(logTag, "configureSession: USBFS_RESET rc=$resetRc (continuing)")
            }
            try {
                Thread.sleep(POST_USB_RESET_SETTLE_MS)
            } catch (_: InterruptedException) {
            }
        }
    }

    override fun consumeLastPreviewStartFailed(): Boolean {
        val failed = lastPreviewStartFailed
        lastPreviewStartFailed = false
        return failed
    }

    override fun consumeLastProbeOpenFailed(): Boolean {
        val failed = lastProbeOpenFailed
        lastProbeOpenFailed = false
        return failed
    }

    override fun describeStartBlocker(
        context: Context,
        usbRepository: UsbCaptureDeviceRepository,
        device: CaptureDevice,
    ): String? {
        if (!usbRepository.hasPermission(device)) {
            return context.getString(R.string.message_permission_required)
        }
        if (usbRepository.resolveUsbDevice(device) == null) {
            return context.getString(R.string.message_usb_device_not_found)
        }
        return null
    }

    override fun bindPreviewSurface(surfaceHandle: Any?) {
        textureView = surfaceHandle as? TextureView ?: return
        val view = textureView ?: return
        view.applyForUsbPreviewLatency()
        view.surfaceTextureListener = surfaceListener
        if (view.isAvailable) {
            val tex = view.surfaceTexture ?: return
            requestStartPreview(tex)
        }
    }

    override fun unbindPreviewSurface() {
        textureView?.surfaceTextureListener = null
        textureView = null
        stopUvcStreamingAsync()
    }

    /**
     * [surface] must be the active TextureView surface (typically obtained on the main thread).
     */
    private fun requestStartPreview(surface: SurfaceTexture) {
        uvcPreviewExecutor.execute {
            startPreviewIfReadyWithSurface(surface)
        }
    }

    override fun probeSupportedSizes(device: CaptureDevice): List<Size> {
        lastProbeOpenFailed = false

        val usbDevice = usbRepository.resolveUsbDevice(device)
        if (usbDevice == null) {
            Log.w(
                probeLogTag,
                "probeSupportedSizes: no UsbDevice for capture id=${device.id} name=${device.name}",
            )
            return emptyList()
        }
        if (!usbRepository.hasPermission(device)) {
            Log.w(
                probeLogTag,
                "probeSupportedSizes: no USB permission capture id=${device.id} name=${device.name} " +
                    "usb=${usbDevice.deviceName}",
            )
            return emptyList()
        }

        val t0 = SystemClock.elapsedRealtime()
        Log.i(
            probeLogTag,
            "probeSupportedSizes: BEGIN capture id=${device.id} name=${device.name} " +
                "usb deviceName=${usbDevice.deviceName} vid=0x${usbDevice.vendorId.toString(16)} " +
                "pid=0x${usbDevice.productId.toString(16)}",
        )

        val descriptorSizes = probeSupportedSizesFromUsbDescriptors(usbDevice)
        if (descriptorSizes.isNotEmpty()) {
            Log.i(
                probeLogTag,
                "probeSupportedSizes: descriptor probe ok count=${descriptorSizes.size} " +
                    "totalMs=${SystemClock.elapsedRealtime() - t0}",
            )
            descriptorSizes.forEachIndexed { index, size ->
                Log.i(
                    probeLogTag,
                    "probeSupportedSizes: descriptor[$index] w=${size.width} h=${size.height} " +
                        "type=${size.type} frame_type=${size.frame_type} index=${size.index} " +
                        "frameIntervalType=${size.frameIntervalType}",
                )
            }
            lastProbeOpenFailed = false
            return descriptorSizes
        }
        Log.w(
            probeLogTag,
            "probeSupportedSizes: descriptor probe empty, falling back to native libuvc open",
        )

        // Drop any preview native camera and the cached UsbDeviceConnection so we never reuse a
        // stuck libusb session (release_interface errno 22 / open result -99).
        stopUvcStreamingBlocking()

        var sawOpenFailure = false
        // Fresh monitor: stop/removal recovery may have destroyed [usbMonitorRef].
        var monitor = ensureUsbMonitor()
        monitor.releaseCachedDevice(usbDevice)
        try {
            Thread.sleep(PROBE_AFTER_RELEASE_MS)
        } catch (_: InterruptedException) {
        }
        val probeResetRc = kernelUsbResetIfPossible(usbDevice)
        if (probeResetRc != 0) {
            Log.w(probeLogTag, "probeSupportedSizes: USBFS_RESET rc=$probeResetRc (continuing)")
        }
        try {
            Thread.sleep(POST_USB_RESET_SETTLE_MS)
        } catch (_: InterruptedException) {
        }

        repeat(PROBE_MAX_ATTEMPTS) { attemptIndex ->
            val attemptNum = attemptIndex + 1
            try {
                if (attemptNum > 1) {
                    Log.i(
                        probeLogTag,
                        "probeSupportedSizes: retry attempt=$attemptNum/$PROBE_MAX_ATTEMPTS after USB reset",
                    )
                    stopUvcStreamingBlocking()
                    monitor = ensureUsbMonitor()
                    Thread.sleep(PROBE_RETRY_DELAY_MS * attemptNum)
                    monitor.releaseCachedDevice(usbDevice)
                    try {
                        Thread.sleep(PROBE_AFTER_RELEASE_MS)
                    } catch (_: InterruptedException) {
                    }
                    val retryResetRc = kernelUsbResetIfPossible(usbDevice)
                    if (retryResetRc != 0) {
                        Log.w(
                            probeLogTag,
                            "probeSupportedSizes: retry USBFS_RESET rc=$retryResetRc (continuing)",
                        )
                    }
                    try {
                        Thread.sleep(POST_USB_RESET_SETTLE_MS)
                    } catch (_: InterruptedException) {
                    }
                }

                monitor.releaseCachedDevice(usbDevice)
                Thread.sleep(PROBE_AFTER_RELEASE_MS)

                val afterOpenMs = SystemClock.elapsedRealtime() - t0
                val controlBlock = monitor.openDevice(usbDevice)
                Log.i(
                    probeLogTag,
                    "probeSupportedSizes: openDevice attempt=$attemptNum afterMs=$afterOpenMs",
                )

                val camera = UVCCamera()
                try {
                    camera.open(controlBlock)
                } catch (openErr: Exception) {
                    sawOpenFailure = true
                    Log.w(
                        probeLogTag,
                        "probeSupportedSizes: UVCCamera.open failed attempt=$attemptNum",
                        openErr,
                    )
                    runCatching { camera.destroy() }
                    monitor.releaseCachedDevice(usbDevice)
                    return@repeat
                }

                try {
                    Log.i(
                        probeLogTag,
                        "probeSupportedSizes: UVCCamera.open ok attempt=$attemptNum totalMs=${SystemClock.elapsedRealtime() - t0}",
                    )

                    Thread.sleep(600)
                    camera.checkSupportFlag(0)
                    Log.i(
                        probeLogTag,
                        "probeSupportedSizes: after sleep+checkSupportFlag totalMs=${SystemClock.elapsedRealtime() - t0}",
                    )

                    logDescriptorJsonToLogcat(camera)

                    val raw = camera.supportedSizeList
                    Log.i(
                        probeLogTag,
                        "probeSupportedSizes: supportedSizeList size=${raw.size} (empty=${raw.isEmpty()}) " +
                            "totalMs=${SystemClock.elapsedRealtime() - t0}",
                    )
                    raw.forEachIndexed { index, size ->
                        Log.i(probeLogTag, "probeSupportedSizes: raw[$index] $size")
                    }

                    val copies = raw.map { Size(it) }
                    copies.forEachIndexed { index, size ->
                        val fpsTry = runCatching { size.getCurrentFrameRate() }
                        Log.i(
                            probeLogTag,
                            "probeSupportedSizes: copy[$index] w=${size.width} h=${size.height} " +
                                "type=${size.type} frame_type=${size.frame_type} index=${size.index} " +
                                "frameIntervalType=${size.frameIntervalType} frameIntervalIndex=${size.frameIntervalIndex} " +
                                "currentFps=${fpsTry.getOrNull()} currentFpsErr=${fpsTry.exceptionOrNull()?.message}",
                        )
                    }

                    if (copies.isEmpty()) {
                        Log.w(
                            probeLogTag,
                            "probeSupportedSizes: supportedSizeList empty after native parse — " +
                                "see ConsolationUvcNative / uvc_descriptors_json chunks above",
                        )
                    }

                    Log.i(
                        probeLogTag,
                        "probeSupportedSizes: END ok copies=${copies.size} totalMs=${SystemClock.elapsedRealtime() - t0}",
                    )
                    lastProbeOpenFailed = false
                    return copies
                } finally {
                    runCatching { camera.destroy() }
                    monitor.releaseCachedDevice(usbDevice)
                }
            } catch (e: Exception) {
                Log.e(
                    probeLogTag,
                    "probeSupportedSizes: attempt $attemptNum failed capture id=${device.id} name=${device.name} " +
                        "afterMs=${SystemClock.elapsedRealtime() - t0}",
                    e,
                )
                runCatching { ensureUsbMonitor().releaseCachedDevice(usbDevice) }
            }
        }

        lastProbeOpenFailed = sawOpenFailure
        Log.e(
            probeLogTag,
            "probeSupportedSizes: FAILED all attempts capture id=${device.id} name=${device.name} openFail=$sawOpenFailure",
        )
        return emptyList()
    }

    override fun getSupportedSizes(): List<Size> {
        val camera = uvcCamera ?: run {
            // If camera is not open, we might need to open it temporarily or wait
            // For now, return empty if not open.
            return emptyList()
        }
        return camera.supportedSizeList
    }

    override fun setPreviewSize(width: Int, height: Int, fps: Int) {
        targetWidth = width
        targetHeight = height
        targetFps = fps
        if (uvcCamera != null) {
            stopUvcStreamingBlocking()
            mainHandler.post {
                val tex = textureView?.surfaceTexture ?: return@post
                requestStartPreview(tex)
            }
        }
    }

    override fun setPreferredPixelFormat(frameFormat: Int?) {
        preferredPixelFormatOverride = frameFormat
    }

    override fun setCaptureAudioVolume(linear: Float) {
        captureAudioVolumeLinear = linear.coerceIn(0f, 1f)
        captureAudioRef.get()?.setVolume(captureAudioVolumeLinear)
    }

    override fun setCaptureAudioFailureListener(listener: (() -> Unit)?) {
        captureAudioFailureListener = listener
    }

    override fun hasReceivedFirstVideoFrame(): Boolean = loggedFirstVideoFrame.get()

    override fun getTelemetry(): TelemetrySnapshot {
        val now = System.currentTimeMillis()
        val elapsed = now - lastTelemetryTime
        if (elapsed >= 1000) {
            val processingStats = uvcCamera?.getAndResetProcessingStats() ?: EMPTY_PROCESSING_STATS
            val nativeFrameCount = processingStats.getOrElse(9) { 0L }.toInt()
            val callbackFrameCount = frameCount.getAndSet(0)
            val observedFrames = if (nativeFrameCount > 0) nativeFrameCount else callbackFrameCount
            actualFps = (observedFrames * 1000 / elapsed).toInt()
            if (nativeFrameCount > 0 && loggedFirstVideoFrame.compareAndSet(false, true)) {
                Log.i(logTag, "playback: first video frame observed via native preview stats")
                val audioUsbDevice = pendingAudioUsbDevice
                val audioCamera = pendingAudioCamera
                if (audioUsbDevice != null && audioCamera != null) {
                    scheduleDeferredUsbAudio(
                        audioUsbDevice,
                        audioCamera,
                        pendingAudioGeneration,
                    )
                }
            }
            lastTelemetryTime = now
            droppedFrames += processingStats.getOrElse(12) { 0L }.toInt()
            val perFrameLatencyMs = nsToMs(processingStats.getOrElse(1) { 0L })
            val avgQueuedFrames = processingStats.getOrElse(13) { 0L } / 1000.0
            lastNativeEndToEndLatencyAvgMs = perFrameLatencyMs * (1.0 + avgQueuedFrames)
            lastNativeQueuedAvgFrames = avgQueuedFrames
            lastNativePayloadAvgKb = bytesToKb(processingStats.getOrElse(10) { 0L })
            lastNativePreviewConvAvgMs = nsToMs(processingStats.getOrElse(14) { 0L })
            lastNativeEndToEndMaxMs = nsToMs(processingStats.getOrElse(15) { 0L })
            lastNativeQueueDeqMaxFrames = processingStats.getOrElse(16) { 0L } / 1000.0
            lastNativeQueueEnqAvgFrames = processingStats.getOrElse(17) { 0L } / 1000.0
            lastNativeQueueEnqMaxFrames = processingStats.getOrElse(18) { 0L } / 1000.0
            lastNativeUvcCbAvgMs = nsToMs(processingStats.getOrElse(19) { 0L })
            lastNativeUvcCbMaxMs = nsToMs(processingStats.getOrElse(20) { 0L })
            lastNativeCbLagAvgMs = nsToMs(processingStats.getOrElse(21) { 0L })
            lastNativeCbLagMaxMs = nsToMs(processingStats.getOrElse(22) { 0L })
            lastNativeCbLagCount = processingStats.getOrElse(23) { 0L }
            lastNativePreCbSkip = processingStats.getOrElse(24) { 0L }
            lastNativeFrameInterval100ns = processingStats.getOrElse(25) { 0L }
            lastNativeAltSetting = processingStats.getOrElse(26) { 0L }
            val nativePublishedCount = processingStats.getOrElse(27) { 0L }
            val publishedDelta = (nativePublishedCount - lastNativePublishedCountRaw).coerceAtLeast(0L)
            lastNativePubFps = publishedDelta * 1000.0 / elapsed.toDouble()
            lastNativePublishedCountRaw = nativePublishedCount
            lastNativeStreamDrop = processingStats.getOrElse(28) { 0L }
            lastNativeIsIsochronous = processingStats.getOrElse(29) { 0L } != 0L
            return TelemetrySnapshot(
                fps = actualFps,
                droppedFrames = droppedFrames,
                width = currentWidth,
                height = currentHeight,
                configuredFps = currentFpsConfigured,
                pixelFormat = currentPixelFormat,
                backendName = telemetryBackendLabel,
                nativeEndToEndLatencyAvgMs = lastNativeEndToEndLatencyAvgMs,
                nativeQueuedAvgFrames = lastNativeQueuedAvgFrames,
                nativePayloadAvgKb = lastNativePayloadAvgKb,
                nativePreviewConvAvgMs = lastNativePreviewConvAvgMs,
                nativeEndToEndMaxMs = lastNativeEndToEndMaxMs,
                nativeQueueDeqMaxFrames = lastNativeQueueDeqMaxFrames,
                nativeQueueEnqAvgFrames = lastNativeQueueEnqAvgFrames,
                nativeQueueEnqMaxFrames = lastNativeQueueEnqMaxFrames,
                nativeUvcCbAvgMs = lastNativeUvcCbAvgMs,
                nativeUvcCbMaxMs = lastNativeUvcCbMaxMs,
                nativeCbLagAvgMs = lastNativeCbLagAvgMs,
                nativeCbLagMaxMs = lastNativeCbLagMaxMs,
                nativeCbLagCount = lastNativeCbLagCount,
                nativePubFps = lastNativePubFps,
                nativePreCbSkip = lastNativePreCbSkip,
                nativeStreamDrop = lastNativeStreamDrop,
                nativeFrameInterval100ns = lastNativeFrameInterval100ns,
                nativeAltSetting = lastNativeAltSetting,
                nativeIsIsochronous = lastNativeIsIsochronous,
            )
        }
        return TelemetrySnapshot(
            fps = actualFps,
            droppedFrames = droppedFrames,
            width = currentWidth,
            height = currentHeight,
            configuredFps = currentFpsConfigured,
            pixelFormat = currentPixelFormat,
            backendName = telemetryBackendLabel,
            nativeEndToEndLatencyAvgMs = lastNativeEndToEndLatencyAvgMs,
            nativeQueuedAvgFrames = lastNativeQueuedAvgFrames,
            nativePayloadAvgKb = lastNativePayloadAvgKb,
            nativePreviewConvAvgMs = lastNativePreviewConvAvgMs,
            nativeEndToEndMaxMs = lastNativeEndToEndMaxMs,
            nativeQueueDeqMaxFrames = lastNativeQueueDeqMaxFrames,
            nativeQueueEnqAvgFrames = lastNativeQueueEnqAvgFrames,
            nativeQueueEnqMaxFrames = lastNativeQueueEnqMaxFrames,
            nativeUvcCbAvgMs = lastNativeUvcCbAvgMs,
            nativeUvcCbMaxMs = lastNativeUvcCbMaxMs,
            nativeCbLagAvgMs = lastNativeCbLagAvgMs,
            nativeCbLagMaxMs = lastNativeCbLagMaxMs,
            nativeCbLagCount = lastNativeCbLagCount,
            nativePubFps = lastNativePubFps,
            nativePreCbSkip = lastNativePreCbSkip,
            nativeStreamDrop = lastNativeStreamDrop,
            nativeFrameInterval100ns = lastNativeFrameInterval100ns,
            nativeAltSetting = lastNativeAltSetting,
            nativeIsIsochronous = lastNativeIsIsochronous,
        )
    }

    override fun setRotation(degrees: Int) {
        rotationDegrees = degrees
        // UVCCamera doesn't seem to have a direct rotation method in JNI, 
        // so we'll handle it in the UI layer by rotating the TextureView.
    }

    override fun setFlip(horizontal: Boolean, vertical: Boolean) {
        flipHorizontal = horizontal
        flipVertical = vertical
        // Same here, handle in UI layer with scaleX/scaleY.
    }

    override fun dispose() {
        textureView?.surfaceTextureListener = null
        textureView = null
        val stopped = uvcPreviewExecutor.submit {
            stopUvcStreamingBody()
        }
        try {
            stopped.get(8, TimeUnit.SECONDS)
        } catch (_: Exception) {
        }
        usbMonitorRef?.destroy()
        usbMonitorRef = null
        uvcPreviewExecutor.shutdown()
    }

    /** Logs full libuvc descriptor JSON in chunks (Logcat line limit). Tag ConsolationUvcProbe. */
    private fun logDescriptorJsonToLogcat(camera: UVCCamera) {
        val json = camera.getUvcDescriptorsJson()
        if (json.isEmpty()) return
        val maxChunk = 3800
        var offset = 0
        var part = 0
        while (offset < json.length) {
            val end = kotlin.math.min(offset + maxChunk, json.length)
            Log.i(probeLogTag, "uvc_descriptors_json part=$part ${json.substring(offset, end)}")
            offset = end
            part++
        }
    }

    /**
     * Posts [UsbCaptureAudioLoop] start on the main looper. Cancels the no-frame fallback once a
     * real schedule runs. Must not block the preview executor.
     */
    private fun scheduleDeferredUsbAudio(
        usbDevice: UsbDevice,
        camera: UVCCamera,
        genAtPreviewStart: Long,
        delayMs: Long = AUDIO_AFTER_FIRST_FRAME_DELAY_MS,
        attempt: Int = 1,
    ) {
        deferredAudioFallbackRunnable?.let { mainHandler.removeCallbacks(it) }
        deferredAudioFallbackRunnable = null
        deferredAudioRunnable?.let { mainHandler.removeCallbacks(it) }
        deferredAudioRunnable = Runnable {
            if (genAtPreviewStart != playbackGeneration) {
                Log.i(
                    logTag,
                    "playback: deferred USB audio skipped stale gen=$genAtPreviewStart " +
                        "current=$playbackGeneration",
                )
                return@Runnable
            }
            if (uvcCamera !== camera) {
                Log.i(logTag, "playback: deferred USB audio skipped camera instance replaced")
                return@Runnable
            }
            val tA = SystemClock.elapsedRealtime()
            Log.i(
                logTag,
                "playback: deferred USB audio firing gen=$genAtPreviewStart " +
                    "attempt=$attempt/$USB_AUDIO_MAX_START_ATTEMPTS delayMs=$delayMs",
            )
            val audioLoop = UsbCaptureAudioLoop(appContext)
            if (!audioLoop.start(usbDevice)) {
                if (attempt < USB_AUDIO_MAX_START_ATTEMPTS) {
                    val retryDelay = USB_AUDIO_RETRY_DELAY_MS * attempt
                    Log.w(
                        usbAudioRouteLogTag,
                        "usb_audio_route audio_loop_start retry_scheduled " +
                            "attempt=$attempt nextAttempt=${attempt + 1} delayMs=$retryDelay " +
                            "videoDeviceName=${usbDevice.deviceName}",
                    )
                    scheduleDeferredUsbAudio(
                        usbDevice,
                        camera,
                        genAtPreviewStart,
                        delayMs = retryDelay,
                        attempt = attempt + 1,
                    )
                } else {
                    Log.w(
                        usbAudioRouteLogTag,
                        "usb_audio_route audio_loop_start giving_up attempts=$attempt " +
                            "videoDeviceName=${usbDevice.deviceName}",
                    )
                    Log.w(logTag, "playback: USB audio start() returned false")
                    captureAudioFailureListener?.invoke()
                }
                return@Runnable
            }
            audioLoop.setVolume(captureAudioVolumeLinear)
            if (genAtPreviewStart != playbackGeneration || uvcCamera !== camera) {
                Log.w(logTag, "playback: USB audio orphan after race gen=$genAtPreviewStart")
                audioLoop.stop()
                return@Runnable
            }
            if (!captureAudioRef.compareAndSet(null, audioLoop)) {
                Log.w(
                    logTag,
                    "playback: USB audio duplicate attach; stopping stray loop gen=$genAtPreviewStart",
                )
                audioLoop.stop()
                return@Runnable
            }
            Log.i(
                logTag,
                "playback: USB audio attached ok extraMs=${SystemClock.elapsedRealtime() - tA}",
            )
        }
        mainHandler.postDelayed(deferredAudioRunnable!!, delayMs)
    }

    /**
     * Must run on [uvcPreviewExecutor] only. Do not use [synchronized] on this object across
     * native start/stop: the main thread posts delayed USB-audio runnables that must never block
     * behind a held monitor (frozen UI when native startPreview blocks).
     */
    private fun startPreviewIfReadyWithSurface(surfaceTexture: SurfaceTexture) {
        val tSession = SystemClock.elapsedRealtime()
        Log.i(
            logTag,
            "playback: startPreviewIfReady enter gen=$playbackGeneration thread=${Thread.currentThread().name}",
        )
        if (previewRunning && uvcCamera != null) {
            Log.i(logTag, "playback: startPreviewIfReady skip already running")
            return
        }

        val captureDevice = preferredDevice ?: run {
            Log.d(logTag, "playback: abort no preferredDevice")
            return
        }
        val usbDevice = usbRepository.resolveUsbDevice(captureDevice) ?: run {
            Log.d(logTag, "playback: abort resolveUsbDevice null")
            return
        }
        if (!usbRepository.hasPermission(captureDevice)) {
            Log.d(logTag, "playback: abort no USB permission")
            return
        }
        if (textureView == null) {
            Log.d(logTag, "playback: abort no textureView")
            return
        }

        try {
            val genAtPreviewStart = playbackGeneration
            monitorCachedUsbDevice = usbDevice
            var t1 = SystemClock.elapsedRealtime()
            val camera = uvcCamera ?: run {
                Log.i(logTag, "playback: opening UVC device…")
                val controlBlock = ensureUsbMonitor().openDevice(usbDevice)
                Log.i(logTag, "playback: openDevice ${SystemClock.elapsedRealtime() - t1}ms")
                t1 = SystemClock.elapsedRealtime()
                val c = UVCCamera()
                c.open(controlBlock)
                Log.i(logTag, "playback: UVCCamera.open ${SystemClock.elapsedRealtime() - t1}ms")
                c
            }
            uvcCamera = camera

            val listed = camera.supportedSizeList
            val fps = if (targetFps > 0) targetFps else 30
            val matched = listed
                .filter { it.width == targetWidth && it.height == targetHeight }
                .minByOrNull { size ->
                    val fpsArray = size.fps ?: return@minByOrNull Float.POSITIVE_INFINITY
                    fpsArray.minOfOrNull { kotlin.math.abs(it - fps.toFloat()) }
                        ?: Float.POSITIVE_INFINITY
                }
            val width = when {
                matched != null -> matched.width
                targetWidth > 0 -> targetWidth
                else -> listed.firstOrNull()?.width ?: UVCCamera.DEFAULT_PREVIEW_WIDTH
            }
            val height = when {
                matched != null -> matched.height
                targetHeight > 0 -> targetHeight
                else -> listed.firstOrNull()?.height ?: UVCCamera.DEFAULT_PREVIEW_HEIGHT
            }
            // Prefer a tight [fps,fps] range so libuvc selects one interval; wide (1,fps) can
            // pick a suboptimal mode when switching only frame rate at fixed resolution.
            val minFps = fps.coerceAtLeast(1)
            val maxFps = fps
            // 1080p60 (and similar) can exceed default isochronous budget on some USB paths; a
            // slightly reduced bandwidth factor forces libuvc to pick a viable alt-setting after
            // switching up from a lower frame rate.
            val bwFactor = previewBandwidthFactor(width, height, fps)

            Log.i(
                logTag,
                "playback: negotiated target ${width}x${height}@${fps} matched=${matched != null} listed=${listed.size} bw=$bwFactor",
            )

            t1 = SystemClock.elapsedRealtime()
            val nativeFrameFormat = matched?.frame_type?.takeIf {
                it == UVCCamera.FRAME_FORMAT_H264 ||
                    it == UVCCamera.FRAME_FORMAT_NV12 ||
                    it == UVCCamera.FRAME_FORMAT_P010
            }
            val lowBandwidthHint = detectLowBandwidthHint(usbDevice)
            if (lowBandwidthHint != null) {
                Log.i(logTag, "playback: usb bandwidth hint low=$lowBandwidthHint")
            } else {
                Log.i(logTag, "playback: usb bandwidth hint unavailable; auto prefers uncompressed")
            }
            val autoOrder = buildAutoFormatOrder(nativeFrameFormat, lowBandwidthHint == true)
            val selectedOrder = preferredPixelFormatOverride?.let { pref ->
                listOf(pref) + autoOrder.filter { it != pref }
            } ?: autoOrder
            val selectedFrameFormat = selectedOrder.firstNotNullOfOrNull { format ->
                trySetPreviewSize(camera, width, height, minFps, maxFps, fps, format, bwFactor).also {
                    if (it != null) {
                        Log.i(logTag, "playback: using requested order format ${frameFormatName(it)}")
                    }
                }
            } ?: run {
                throw IllegalStateException("no compatible preview format found for ${width}x${height}@${fps}")
            }

            currentWidth = width
            currentHeight = height
            currentFpsConfigured = fps
            currentPixelFormat = frameFormatName(selectedFrameFormat)

            surfaceTexture.setDefaultBufferSize(width, height)
            if (selectedFrameFormat == UVCCamera.FRAME_FORMAT_H264) {
                t1 = SystemClock.elapsedRealtime()
                startH264Decoder(surfaceTexture, width, height)
                Log.i(logTag, "playback: startH264Decoder ${SystemClock.elapsedRealtime() - t1}ms")
            } else {
                t1 = SystemClock.elapsedRealtime()
                camera.setPreviewTexture(surfaceTexture)
                Log.i(logTag, "playback: setPreviewTexture ${SystemClock.elapsedRealtime() - t1}ms")
            }

            loggedFirstVideoFrame.set(false)
            pendingAudioUsbDevice = usbDevice
            pendingAudioCamera = camera
            pendingAudioGeneration = genAtPreviewStart
            if (selectedFrameFormat == UVCCamera.FRAME_FORMAT_H264) {
                camera.setPreviewFrameCallback({ frame ->
                    queueH264Frame(frame)
                    frameCount.incrementAndGet()
                    if (loggedFirstVideoFrame.compareAndSet(false, true)) {
                        Log.i(
                            logTag,
                            "playback: first video frame callback msSinceSessionStart=" +
                                "${SystemClock.elapsedRealtime() - tSession}",
                        )
                        // Start USB capture audio only after UVC isochronous streaming is active; opening
                        // AudioRecord earlier (fixed delay after startPreview) races USB bandwidth / routing
                        // and often breaks when the first frame appears on composite capture cards.
                        scheduleDeferredUsbAudio(usbDevice, camera, genAtPreviewStart)
                    }
                }, UVCCamera.PIXEL_FORMAT_RAW)
            } else {
                camera.setPreviewFrameCallback(null, 0)
            }

            t1 = SystemClock.elapsedRealtime()
            camera.startPreview()
            previewRunning = true
            Log.i(logTag, "playback: startPreview native ${SystemClock.elapsedRealtime() - t1}ms")

            uvcCamera = camera

            deferredAudioRunnable?.let { mainHandler.removeCallbacks(it) }
            deferredAudioFallbackRunnable?.let { mainHandler.removeCallbacks(it) }
            deferredAudioFallbackRunnable = Runnable {
                deferredAudioFallbackRunnable = null
                if (genAtPreviewStart != playbackGeneration) return@Runnable
                if (captureAudioRef.get() != null) return@Runnable
                if (uvcCamera !== camera) return@Runnable
                Log.i(
                    logTag,
                    "playback: USB audio fallback — no attach before ${USB_AUDIO_FALLBACK_MS}ms " +
                        "(frame callback may be absent); attempting start",
                )
                scheduleDeferredUsbAudio(usbDevice, camera, genAtPreviewStart, delayMs = 0L)
            }
            mainHandler.postDelayed(deferredAudioFallbackRunnable!!, USB_AUDIO_FALLBACK_MS)

            Log.i(
                logTag,
                "playback: startPreviewIfReady ok totalMs=${SystemClock.elapsedRealtime() - tSession}",
            )
        } catch (failure: Exception) {
            Log.e(logTag, "UVC preview failed", failure)
            lastPreviewStartFailed = true
            previewRunning = false
            stopUvcStreamingBody()
        }
    }

    private fun stopUvcStreamingAsync() {
        uvcPreviewExecutor.execute {
            stopUvcStreamingBody()
        }
    }

    /**
     * Waits until preview teardown finishes on [uvcPreviewExecutor]. Call from IO/probe threads,
     * never from the main thread with a long timeout.
     */
    private fun stopUvcStreamingBlocking() {
        if (Thread.currentThread().name == UVC_PREVIEW_THREAD_NAME) {
            stopUvcStreamingBody()
            return
        }
        val future = uvcPreviewExecutor.submit {
            stopUvcStreamingBody()
        }
        try {
            future.get(STOP_JOIN_TIMEOUT_MS, TimeUnit.MILLISECONDS)
        } catch (e: Exception) {
            Log.e(logTag, "playback: stopUvcStreamingBlocking wait failed", e)
            future.cancel(true)
            abandonNativeState("stop timeout")
        }
    }

    /**
     * When [stopUvcStreamingBody] cannot be trusted to complete (unplug during blocking JNI,
     * wedged libusb, or an explicit hot-removal fast path), abandon the executor/native refs
     * and rebuild [USBMonitor].
     * Otherwise the next [probeSupportedSizes]/play keeps a stale [UVCCamera] or a destroyed
     * monitor ([UsbDevice.openDevice] on "already destroyed").
     */
    private fun abandonNativeState(reason: String) {
        Log.w(logTag, "playback: abandonNativeState reason=$reason — executor + native refs + USBMonitor reset")
        val oldExec = uvcPreviewExecutor
        uvcPreviewExecutor = newPreviewExecutor()
        oldExec.shutdownNow()

            playbackGeneration++
            deferredAudioRunnable?.let { mainHandler.removeCallbacks(it) }
            deferredAudioRunnable = null
            deferredAudioFallbackRunnable?.let { mainHandler.removeCallbacks(it) }
            deferredAudioFallbackRunnable = null
            pendingAudioUsbDevice = null
            pendingAudioCamera = null
            pendingAudioGeneration = 0L
            runCatching { captureAudioRef.getAndSet(null)?.stop() }

        previewRunning = false
        uvcCamera = null
        stopH264Decoder()
        loggedFirstVideoFrame.set(false)
        resetTelemetryCounters()
        monitorCachedUsbDevice = null

        val deadMonitor = usbMonitorRef
        usbMonitorRef = null
        runCatching { deadMonitor?.destroy() }
            .onFailure { Log.w(logTag, "USBMonitor.destroy during abandonNativeState", it) }
    }

    private fun stopUvcStreamingBody() {
        val tWhole = SystemClock.elapsedRealtime()
        playbackGeneration++
        deferredAudioRunnable?.let { mainHandler.removeCallbacks(it) }
        deferredAudioRunnable = null
        deferredAudioFallbackRunnable?.let { mainHandler.removeCallbacks(it) }
        deferredAudioFallbackRunnable = null
        pendingAudioUsbDevice = null
        pendingAudioCamera = null
        pendingAudioGeneration = 0L
        Log.i(
            logTag,
            "playback: stopUvcStreaming gen=$playbackGeneration thread=${Thread.currentThread().name}",
        )
        previewRunning = false

        var t0 = SystemClock.elapsedRealtime()
        captureAudioRef.getAndSet(null)?.stop()
        Log.i(logTag, "playback: captureAudio.stop ${SystemClock.elapsedRealtime() - t0}ms")
        stopH264Decoder()

        val camera = uvcCamera ?: run {
            Log.i(logTag, "playback: stop no uvcCamera instance")
            loggedFirstVideoFrame.set(false)
            resetTelemetryCounters()
            monitorCachedUsbDevice = null
            return
        }
        t0 = SystemClock.elapsedRealtime()
        runCatching {
            camera.stopPreview()
            Log.i(logTag, "playback: camera.stopPreview ${SystemClock.elapsedRealtime() - t0}ms")
        }.onFailure { Log.e(logTag, "playback: stopPreview failed", it) }

        t0 = SystemClock.elapsedRealtime()
        runCatching {
            camera.destroy()
            Log.i(logTag, "playback: camera.destroy ${SystemClock.elapsedRealtime() - t0}ms")
        }.onFailure { Log.e(logTag, "playback: destroy failed", it) }

        uvcCamera = null
        loggedFirstVideoFrame.set(false)
        resetTelemetryCounters()
        
        val dev = monitorCachedUsbDevice
        monitorCachedUsbDevice = null
        if (dev != null) {
            runCatching {
                ensureUsbMonitor().releaseCachedDevice(dev)
                Thread.sleep(POST_USB_RELEASE_SETTLE_MS)
                val resetRc = kernelUsbResetIfPossible(dev)
                if (resetRc != 0) {
                    Log.w(logTag, "playback: USBFS_RESET on stop rc=$resetRc")
                }
            }.onFailure { Log.e(logTag, "playback: USB reset on stop failed", it) }
        }
        
        Log.i(
            logTag,
            "playback: stopUvcStreaming done totalMs=${SystemClock.elapsedRealtime() - tWhole}",
        )
    }

    private fun startH264Decoder(surfaceTexture: SurfaceTexture, width: Int, height: Int) {
        stopH264Decoder()
        val surface = Surface(surfaceTexture)
        val format = MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, width, height)
        val decoder = MediaCodec.createDecoderByType(MediaFormat.MIMETYPE_VIDEO_AVC)
        decoder.configure(format, surface, null, 0)
        decoder.start()
        synchronized(h264DecoderLock) {
            h264OutputSurface = surface
            h264Decoder = decoder
            h264FramePtsUs = 0L
        }
        Log.i(logTag, "playback: H264 MediaCodec decoder started ${width}x$height")
    }

    private fun stopH264Decoder() {
        var decoder: MediaCodec? = null
        var surface: Surface? = null
        synchronized(h264DecoderLock) {
            decoder = h264Decoder
            surface = h264OutputSurface
            h264Decoder = null
            h264OutputSurface = null
            h264FramePtsUs = 0L
        }
        runCatching { decoder?.stop() }
        runCatching { decoder?.release() }
        runCatching { surface?.release() }
    }

    private fun queueH264Frame(encodedFrame: ByteBuffer) {
        synchronized(h264DecoderLock) {
            val decoder = h264Decoder ?: return
            val inputIndex = try {
                decoder.dequeueInputBuffer(0)
            } catch (e: Exception) {
                Log.w(logTag, "playback: H264 dequeueInputBuffer failed", e)
                return
            }
            if (inputIndex >= 0) {
                val input = decoder.getInputBuffer(inputIndex)
                if (input != null) {
                    val src = encodedFrame.duplicate()
                    input.clear()
                    if (src.remaining() <= input.remaining()) {
                        input.put(src)
                        val pts = h264FramePtsUs
                        h264FramePtsUs += if (currentFpsConfigured > 0) {
                            1_000_000L / currentFpsConfigured
                        } else {
                            33_333L
                        }
                        decoder.queueInputBuffer(inputIndex, 0, input.position(), pts, 0)
                    } else {
                        decoder.queueInputBuffer(inputIndex, 0, 0, h264FramePtsUs, 0)
                        Log.w(logTag, "playback: dropped oversized H264 frame bytes=${src.remaining()}")
                    }
                }
            }

            val info = MediaCodec.BufferInfo()
            while (true) {
                val outputIndex = try {
                    decoder.dequeueOutputBuffer(info, 0)
                } catch (e: Exception) {
                    Log.w(logTag, "playback: H264 dequeueOutputBuffer failed", e)
                    return
                }
                if (outputIndex >= 0) {
                    decoder.releaseOutputBuffer(outputIndex, true)
                } else if (outputIndex == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                    continue
                } else {
                    break
                }
            }
        }
    }

    private fun frameFormatName(frameFormat: Int): String = when (frameFormat) {
        UVCCamera.FRAME_FORMAT_YUYV -> "YUYV"
        UVCCamera.FRAME_FORMAT_MJPEG -> "MJPEG"
        UVCCamera.FRAME_FORMAT_H264 -> "H264"
        UVCCamera.FRAME_FORMAT_NV12 -> "NV12"
        UVCCamera.FRAME_FORMAT_P010 -> "P010"
        else -> "format-$frameFormat"
    }

    private fun trySetPreviewSize(
        camera: UVCCamera,
        width: Int,
        height: Int,
        minFps: Int,
        maxFps: Int,
        fps: Int,
        frameFormat: Int,
        bwFactor: Float,
    ): Int? {
        val t0 = SystemClock.elapsedRealtime()
        return try {
            try {
                camera.setPreviewSize(width, height, minFps, maxFps, frameFormat, bwFactor)
            } catch (e: Exception) {
                Log.i(
                    logTag,
                    "playback: ${frameFormatName(frameFormat)} tight fps range failed, retry 1..$fps",
                    e,
                )
                camera.setPreviewSize(width, height, 1, maxFps, frameFormat, bwFactor)
            }
            Log.i(
                logTag,
                "playback: setPreviewSize ${frameFormatName(frameFormat)} ok " +
                    "${SystemClock.elapsedRealtime() - t0}ms",
            )
            frameFormat
        } catch (e: Exception) {
            Log.w(logTag, "playback: ${frameFormatName(frameFormat)} setPreviewSize failed", e)
            null
        }
    }

    private fun buildAutoFormatOrder(nativeFrameFormat: Int?, preferCompressed: Boolean): List<Int> {
        val out = mutableListOf<Int>()
        // Keep native hints, but keep deterministic AUTO fallback priority.
        nativeFrameFormat
            ?.takeUnless { it == UVCCamera.FRAME_FORMAT_YUYV }
            ?.let { out.add(it) }
        if (preferCompressed) {
            out.add(UVCCamera.FRAME_FORMAT_MJPEG)
            out.add(UVCCamera.FRAME_FORMAT_NV12)
            out.add(UVCCamera.FRAME_FORMAT_YUYV)
            out.add(UVCCamera.FRAME_FORMAT_P010)
        } else {
            out.add(UVCCamera.FRAME_FORMAT_NV12)
            out.add(UVCCamera.FRAME_FORMAT_YUYV)
            out.add(UVCCamera.FRAME_FORMAT_P010)
            out.add(UVCCamera.FRAME_FORMAT_MJPEG)
        }
        return out.distinct()
    }

    /**
     * Uses device descriptor bcdUSB as a hint only:
     * - true: USB 2.x or older advertised capability (prefer compressed in auto mode)
     * - false: USB 3.x+ capability (prefer uncompressed in auto mode)
     * - null: unknown (prefer uncompressed in auto mode)
     */
    private fun detectLowBandwidthHint(usbDevice: UsbDevice): Boolean? {
        val usbMgr = appContext.getSystemService(Context.USB_SERVICE) as? UsbManager ?: return null
        if (!usbMgr.hasPermission(usbDevice)) return null
        var conn: UsbDeviceConnection? = null
        return try {
            conn = usbMgr.openDevice(usbDevice) ?: return null
            val raw = conn.rawDescriptors ?: return null
            if (raw.size < 4) return null
            val bcdUsb = (u8(raw, 2) or (u8(raw, 3) shl 8))
            bcdUsb < 0x0300
        } catch (_: Exception) {
            null
        } finally {
            runCatching { conn?.close() }
        }
    }

    private fun probeSupportedSizesFromUsbDescriptors(usbDevice: UsbDevice): List<Size> {
        val usbMgr = appContext.getSystemService(Context.USB_SERVICE) as? UsbManager ?: return emptyList()
        if (!usbMgr.hasPermission(usbDevice)) return emptyList()
        var conn: UsbDeviceConnection? = null
        return try {
            conn = usbMgr.openDevice(usbDevice) ?: return emptyList()
            val raw = conn.rawDescriptors ?: return emptyList()
            if (raw.isEmpty()) return emptyList()
            parseUvcFrameSizesFromRawDescriptors(raw)
        } catch (e: Exception) {
            Log.w(probeLogTag, "probeSupportedSizesFromUsbDescriptors: parse failed", e)
            emptyList()
        } finally {
            runCatching { conn?.close() }
        }
    }

    private fun parseUvcFrameSizesFromRawDescriptors(raw: ByteArray): List<Size> {
        val out = mutableListOf<Size>()
        val seen = HashSet<String>()
        var currentFrameType = UVCCamera.FRAME_FORMAT_MJPEG
        var i = 0
        while (i + 2 < raw.size) {
            val len = u8(raw, i)
            if (len <= 2 || i + len > raw.size) {
                i++
                continue
            }
            val descriptorType = u8(raw, i + 1)
            val subtype = u8(raw, i + 2)
            if (descriptorType == USB_DT_CS_INTERFACE) {
                when (subtype) {
                    VS_FORMAT_UNCOMPRESSED ->
                        currentFrameType = parseFrameTypeFromFormatGuid(raw, i, len, subtype)
                            ?: UVCCamera.FRAME_FORMAT_YUYV
                    VS_FORMAT_MJPEG -> currentFrameType = UVCCamera.FRAME_FORMAT_MJPEG
                    VS_FORMAT_FRAME_BASED ->
                        currentFrameType = parseFrameTypeFromFormatGuid(raw, i, len, subtype)
                            ?: UVCCamera.FRAME_FORMAT_H264
                    VS_FRAME_UNCOMPRESSED, VS_FRAME_MJPEG, VS_FRAME_FRAME_BASED -> {
                        val parsed = parseFrameDescriptor(raw, i, len, subtype, currentFrameType)
                        if (parsed != null) {
                            val intervalsHash = parsed.intervals?.contentHashCode() ?: 0
                            val key = "${parsed.width}x${parsed.height}:${parsed.frame_type}:${parsed.frameIntervalType}:$intervalsHash"
                            if (seen.add(key)) out.add(parsed)
                        }
                    }
                }
            }
            i += len
        }
        return out.sortedWith(
            compareByDescending<Size> { it.width * it.height }.thenByDescending {
                it.fps?.maxOrNull() ?: 0f
            },
        )
    }

    /**
     * Parse format GUID from VS_FORMAT_UNCOMPRESSED / VS_FORMAT_FRAME_BASED and map known values
     * to our preview frame format constants.
     */
    private fun parseFrameTypeFromFormatGuid(
        raw: ByteArray,
        offset: Int,
        len: Int,
        subtype: Int,
    ): Int? {
        if (subtype != VS_FORMAT_UNCOMPRESSED && subtype != VS_FORMAT_FRAME_BASED) return null
        // For these format descriptors, guidFormat starts at byte 5 (16 bytes).
        if (len < 21 || offset + 21 > raw.size) return null
        val fourcc = String(raw, offset + 5, 4, StandardCharsets.US_ASCII)
        return when (fourcc) {
            "YUY2", "UYVY", "YVYU" -> UVCCamera.FRAME_FORMAT_YUYV
            "NV12" -> UVCCamera.FRAME_FORMAT_NV12
            "P010" -> UVCCamera.FRAME_FORMAT_P010
            "H264", "AVC1" -> UVCCamera.FRAME_FORMAT_H264
            else -> null
        }
    }

    private fun parseFrameDescriptor(
        raw: ByteArray,
        offset: Int,
        len: Int,
        subtype: Int,
        frameType: Int,
    ): Size? {
        val minimumLen = when (subtype) {
            VS_FRAME_FRAME_BASED -> 34
            else -> 26
        }
        if (len < minimumLen) return null
        val frameIndex = u8(raw, offset + 3)
        val width = u16(raw, offset + 5)
        val height = u16(raw, offset + 7)
        if (width <= 0 || height <= 0) return null
        val intervalTypeOffset = when (subtype) {
            VS_FRAME_FRAME_BASED -> 33
            else -> 25
        }
        if (offset + intervalTypeOffset >= raw.size) return null
        val frameIntervalType = u8(raw, offset + intervalTypeOffset)
        val intervalsStart = offset + intervalTypeOffset + 1
        val intervals = if (frameIntervalType > 0) {
            val needed = frameIntervalType * 4
            if (intervalsStart + needed > offset + len || intervalsStart + needed > raw.size) return null
            IntArray(frameIntervalType) { idx -> u32(raw, intervalsStart + idx * 4) }
                .filter { it > 0 }
                .toIntArray()
        } else {
            if (intervalsStart + 12 > offset + len || intervalsStart + 12 > raw.size) return null
            intArrayOf(
                u32(raw, intervalsStart),
                u32(raw, intervalsStart + 4),
                u32(raw, intervalsStart + 8),
            ).filter { it > 0 }.toIntArray()
        }
        if (intervals.isEmpty()) return null
        val nativeType = when (subtype) {
            VS_FRAME_UNCOMPRESSED -> VS_FORMAT_UNCOMPRESSED
            VS_FRAME_MJPEG -> VS_FORMAT_MJPEG
            else -> VS_FORMAT_FRAME_BASED
        }
        return Size(nativeType, frameType, frameIndex, width, height, intervals)
    }

    private fun u8(raw: ByteArray, offset: Int): Int = raw[offset].toInt() and 0xFF
    private fun u16(raw: ByteArray, offset: Int): Int = u8(raw, offset) or (u8(raw, offset + 1) shl 8)
    private fun u32(raw: ByteArray, offset: Int): Int =
        u8(raw, offset) or
            (u8(raw, offset + 1) shl 8) or
            (u8(raw, offset + 2) shl 16) or
            (u8(raw, offset + 3) shl 24)

    private fun resetTelemetryCounters() {
        droppedFrames = 0
        frameCount.set(0)
        lastTelemetryTime = System.currentTimeMillis()
        actualFps = 0
        lastNativeEndToEndLatencyAvgMs = 0.0
        lastNativeQueuedAvgFrames = 0.0
        lastNativePayloadAvgKb = 0.0
        lastNativePreviewConvAvgMs = 0.0
        lastNativeEndToEndMaxMs = 0.0
        lastNativeQueueDeqMaxFrames = 0.0
        lastNativeQueueEnqAvgFrames = 0.0
        lastNativeQueueEnqMaxFrames = 0.0
        lastNativeUvcCbAvgMs = 0.0
        lastNativeUvcCbMaxMs = 0.0
        lastNativeCbLagAvgMs = 0.0
        lastNativeCbLagMaxMs = 0.0
        lastNativeCbLagCount = 0L
        lastNativePubFps = 0.0
        lastNativePreCbSkip = 0L
        lastNativeStreamDrop = 0L
        lastNativeFrameInterval100ns = 0L
        lastNativeAltSetting = 0L
        lastNativeIsIsochronous = false
        lastNativePublishedCountRaw = 0L
    }

    private fun nsToMs(ns: Long): Double = ns / 1_000_000.0
    private fun bytesToKb(bytes: Long): Double = bytes / 1024.0

    /** libuvc bandwidth hint [0..1]; lower leaves headroom when stepping up to high fps at HD. */
    private fun previewBandwidthFactor(width: Int, height: Int, fps: Int): Float {
        val pixels = width.toLong() * height.toLong()
        val heavy = pixels >= 1920L * 1080L && fps >= 50
        return if (heavy) HIGH_FPS_BANDWIDTH_FACTOR else 1.0f
    }

    companion object {
        private const val logTag: String = "UvcLibPreview"
        private val EMPTY_PROCESSING_STATS = LongArray(UVCCamera.PROCESSING_STATS_COUNT)

        /** Filter: `adb logcat -s ConsolationUvcProbe:I` */
        private const val probeLogTag: String = "ConsolationUvcProbe"
        private const val usbAudioRouteLogTag: String = "ConsolationUsbAudio"

        private const val UVC_PREVIEW_THREAD_NAME = "ConsolationUvcPreview"

        private const val STOP_JOIN_TIMEOUT_MS = 25_000L

        /** After [USBMonitor.releaseCachedDevice]; helps the gadget exit the prior streaming budget. */
        private const val POST_USB_RELEASE_SETTLE_MS = 80L

        /** After kernel [UVCCamera.ioctlUsbReset]; enumeration and gadget init need time to settle. */
        private const val POST_USB_RESET_SETTLE_MS = 250L

        /** Used when UVC negotiate runs at 1080p50+ so MJPEG alt-settings stay within USB budget. */
        private const val HIGH_FPS_BANDWIDTH_FACTOR = 0.85f

        /**
         * Brief settle after the first decoded frame so UVC isochronous streaming is active before
         * opening [android.media.AudioRecord] on the same composite USB device (avoids races with
         * fixed-delay startup).
         */
        private const val AUDIO_AFTER_FIRST_FRAME_DELAY_MS = 120L

        /**
         * If the first-frame callback never attaches USB audio (unlikely), retry once after this
         * delay so capture-only sessions still get sound when possible.
         */
        private const val USB_AUDIO_FALLBACK_MS = 8_000L
        private const val USB_AUDIO_MAX_START_ATTEMPTS = 5
        private const val USB_AUDIO_RETRY_DELAY_MS = 750L

        private const val PROBE_MAX_ATTEMPTS = 3

        /** Brief pause after closing the cached USB connection so the next openDevice() is clean. */
        private const val PROBE_AFTER_RELEASE_MS = 120L

        /** Extra backoff between probe retries when the card returns open failures. */
        private const val PROBE_RETRY_DELAY_MS = 350L

        private const val USB_DT_CS_INTERFACE = 0x24
        private const val VS_FORMAT_UNCOMPRESSED = 0x04
        private const val VS_FRAME_UNCOMPRESSED = 0x05
        private const val VS_FORMAT_MJPEG = 0x06
        private const val VS_FRAME_MJPEG = 0x07
        private const val VS_FORMAT_FRAME_BASED = 0x10
        private const val VS_FRAME_FRAME_BASED = 0x11
    }
}
