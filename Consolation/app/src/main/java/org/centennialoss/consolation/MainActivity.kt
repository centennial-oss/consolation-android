package org.centennialoss.consolation

import android.Manifest
import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.content.Intent
import android.content.SharedPreferences
import android.content.pm.PackageManager
import android.graphics.Color as AndroidColor
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.app.PictureInPictureParams
import android.content.res.Configuration
import android.text.SpannableString
import android.text.style.ForegroundColorSpan
import android.view.ContextThemeWrapper
import android.view.Menu
import android.view.TextureView
import android.view.View
import android.view.WindowManager
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.widget.PopupMenu as AppCompatPopupMenu
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.RowScope
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.RadioButton
import androidx.compose.material3.RadioButtonDefaults
import androidx.compose.material3.Slider
import androidx.compose.material3.SliderDefaults
import androidx.compose.material3.Switch
import androidx.compose.material3.SwitchDefaults
import androidx.compose.material3.Text
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.key
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.core.content.ContextCompat
import androidx.core.view.doOnLayout
import androidx.core.view.isVisible
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.lifecycleScope
import androidx.lifecycle.repeatOnLifecycle
import com.google.android.material.snackbar.Snackbar
import org.centennialoss.consolation.uvc.Size
import org.centennialoss.consolation.capture.DeviceCompatibilityIssue
import org.centennialoss.consolation.capture.DeviceCompatibilityIssues
import org.centennialoss.consolation.capture.NoopCaptureEngine
import org.centennialoss.consolation.core.capture.CaptureDevice
import org.centennialoss.consolation.core.capture.CaptureState
import org.centennialoss.consolation.preview.backend.UsbVideoPreviewBackend
import org.centennialoss.consolation.preview.backend.UsbVideoPreviewBackendFactory
import org.centennialoss.consolation.usb.UsbCaptureDeviceRepository
import org.centennialoss.consolation.uvc.UVCCamera
import org.centennialoss.consolation.logging.AppLog as Log
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlin.math.abs
import java.util.Locale
import kotlin.math.roundToInt

private val ConsolationColorScheme = darkColorScheme(
    primary = Color(0xFFCC11BB),
    secondary = Color(0xFFCC11BB),
)

class MainActivity : ComponentActivity() {
    private lateinit var previewTexture: TextureView
    private lateinit var rootView: View
    private lateinit var deviceRepository: UsbCaptureDeviceRepository
    private lateinit var previewBackend: UsbVideoPreviewBackend
    private val captureEngine = NoopCaptureEngine()

    private lateinit var prefs: SharedPreferences

    private var selectedDevice: CaptureDevice? by mutableStateOf(null)

    /** Raw formats from the last successful probe for the current device (independent of open camera). */
    private var probedFormatSizes: List<Size> = emptyList()
    /** Formats exactly as reported by probe (before debug-only unsafe synthesis). */
    private var probedFormatSizesReported: List<Size> = emptyList()

    /** User choice: width, height, and frame interval index into [Size.fps]. */
    private var selectedFormat: Size? by mutableStateOf(null)
    private var selectedPixelFormatPreference: PixelFormatPreference by mutableStateOf(PixelFormatPreference.AUTO)

    private var permissionTimeoutJob: Job? = null
    private var telemetryJob: Job? = null
    private var controlsAutoHideJob: Job? = null
    private var connectingWatchdogJob: Job? = null
    private var hasRetriedConnectingSession = false
    private var lastUsbPermissionGrantedAtMs = 0L

    private var lastResolutionRefreshDeviceId: String? = null
    private var lastResolutionRefreshHadUsbPermission: Boolean = false
    private var resolutionProbeJob: Job? = null
    private var probingResolutionDeviceId: String? = null
    private var isResolutionProbeInProgress by mutableStateOf(false)
    private var isPlayActionInProgress by mutableStateOf(false)
    private var autoStartPlaybackJob: Job? = null

    // Settings
    private var isStatsVisible by mutableStateOf(false)
    private var statsPosition by mutableStateOf(StatsPosition.OFF)
    private var isLowFpsWarningEnabled by mutableStateOf(true)
    private var isDebugStatsEnabled by mutableStateOf(false)
    private var isPipEnabled by mutableStateOf(true)
    private var currentRotation by mutableIntStateOf(0)
    private var isFlippedHorizontal by mutableStateOf(false)
    private var isFlippedVertical by mutableStateOf(false)
    private var currentZoom by mutableIntStateOf(0)

    private var audioVolumePercent by mutableIntStateOf(100)
    private var audioMuted by mutableStateOf(false)
    private var volumeBeforeMute = 100

    private var lowFpsBelowThresholdSinceMs: Long = 0L
    private var lastTelemetryLogAtMs: Long = 0L

    private var devicesUi: List<CaptureDevice> by mutableStateOf(emptyList())
    private var isPlaybackRunningUi by mutableStateOf(false)
    private var areControlsVisible by mutableStateOf(false)
    private var isConnectingVisible by mutableStateOf(false)
    private var isLowFpsWarningVisible by mutableStateOf(false)
    private var statsOverlayText by mutableStateOf("")
    private var resolutionDropdownText by mutableStateOf("")
    private var permissionNoticeText by mutableStateOf("")
    private var showPermissionNotice by mutableStateOf(false)
    private var showUsbPermissionButton by mutableStateOf(false)
    private var canPlaySelection by mutableStateOf(false)
    private var showResolutionPicker by mutableStateOf(false)
    private var activeSheet: ActiveSheet? by mutableStateOf(null)
    private var blockingErrorMessage: String? by mutableStateOf(null)
    private var previewAspectRatio by mutableFloatStateOf(16f / 9f)
    private var previewTextureGeneration by mutableIntStateOf(0)

    private enum class ActiveSheet { HELP, ABOUT, SETTINGS, COMPATIBILITY, LOW_FPS, AUDIO_FAILURE }

    enum class StatsPosition { OFF, BOTTOM_LEFT, BOTTOM_RIGHT }
    private enum class PixelFormatPreference(val prefValue: String, val frameFormat: Int?) {
        AUTO("auto", null),
        H264("h264", UVCCamera.FRAME_FORMAT_H264),
        NV12("nv12", UVCCamera.FRAME_FORMAT_NV12),
        YUYV("yuyv", UVCCamera.FRAME_FORMAT_YUYV),
        P010("p010", UVCCamera.FRAME_FORMAT_P010),
        YU12("yu12", UVCCamera.FRAME_FORMAT_YU12),
        BGR3("bgr3", UVCCamera.FRAME_FORMAT_BGR3),
        MJPEG("mjpeg", UVCCamera.FRAME_FORMAT_MJPEG),
    }

    private enum class RuntimePermissionAction { REQUEST_USB_PERMISSION, START_WATCH }

    private var pendingRuntimePermissionAction: RuntimePermissionAction? = null
    private var startWatchAfterUsbPermission = false

    private val requestCameraRecordForWatch = registerForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions(),
    ) { results ->
        val action = pendingRuntimePermissionAction
        pendingRuntimePermissionAction = null
        val pending = selectedDevice
        if (pending == null) {
            startWatchAfterUsbPermission = false
            updatePlayActionInProgress(false)
            return@registerForActivityResult
        }
        if (!hasRuntimeCameraPermission() && results[Manifest.permission.CAMERA] != true) {
            startWatchAfterUsbPermission = false
            updatePlayActionInProgress(false)
            showMessage(getString(R.string.message_camera_permission_required_for_usb))
            return@registerForActivityResult
        }
        if (!hasRuntimeRecordAudioPermission() && results[Manifest.permission.RECORD_AUDIO] != true) {
            startWatchAfterUsbPermission = false
            updatePlayActionInProgress(false)
            showMessage(getString(R.string.message_runtime_perms_for_watch))
            return@registerForActivityResult
        }
        when (action) {
            RuntimePermissionAction.REQUEST_USB_PERMISSION -> requestUsbPermissionForSelection(autoStartAfterGrant = false)
            RuntimePermissionAction.START_WATCH -> lifecycleScope.launch {
                startWatchSession(pending)
            }
            null -> Unit
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        prefs = getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
        loadSettingsFromPrefs()

        deviceRepository = UsbCaptureDeviceRepository(this)
        previewBackend = UsbVideoPreviewBackendFactory.create(this, deviceRepository)
        previewBackend.setCaptureAudioFailureListener {
            showCaptureAudioFailureDialog()
        }

        setContent {
            MaterialTheme(colorScheme = ConsolationColorScheme) {
                MainScreen()
            }
        }
        rootView = window.decorView.findViewById(android.R.id.content)
        observeState()

        deviceRepository.refreshAfterUsbIntent(intent)
        updateSystemUiForPlayback(isPlaybackActive = false)
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus && captureEngine.state.value is CaptureState.Running) {
            updateSystemUiForPlayback(isPlaybackActive = true)
        }
    }

    override fun onStart() {
        super.onStart()
        deviceRepository.start()
        if (captureEngine.state.value is CaptureState.Running) {
            Log.i(
                PLAYBACK_DIAG_TAG,
                "activity onStart while running -> rebind preview surface",
            )
            if (::previewTexture.isInitialized) {
                previewBackend.bindPreviewSurface(previewTexture)
            }
            applyAudioVolumeFromUi()
        }
    }

    override fun onStop() {
        super.onStop()
        if (captureEngine.state.value is CaptureState.Running) {
            Log.i(
                PLAYBACK_DIAG_TAG,
                "activity onStop while running -> pause native preview without ending session",
            )
            previewBackend.unbindPreviewSurface()
        }
        deviceRepository.stop()
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        deviceRepository.refreshAfterUsbIntent(intent)
    }

    private fun selectDevice(device: CaptureDevice?) {
        if (device?.id != selectedDevice?.id) {
            selectedFormat = null
            selectedPixelFormatPreference = PixelFormatPreference.AUTO
            probedFormatSizes = emptyList()
            probedFormatSizesReported = emptyList()
            resolutionDropdownText = ""
        }
        selectedDevice = device
        lastResolutionRefreshDeviceId = null
        refreshResolutions()
        updateStartupActions()
    }

    private fun stopPlaybackFromControls() {
        lifecycleScope.launch {
            val t0 = android.os.SystemClock.elapsedRealtime()
            cancelConnectingWatchdog()
            hasRetriedConnectingSession = false
            Log.i(
                PLAYBACK_DIAG_TAG,
                "stop tap → stopWatching first (UI) thread=${Thread.currentThread().name}",
            )
            captureEngine.stopWatching()
            Log.i(
                PLAYBACK_DIAG_TAG,
                "stop stopWatching done ms=${android.os.SystemClock.elapsedRealtime() - t0}",
            )
            withContext(Dispatchers.IO) {
                previewBackend.unbindPreviewSurfaceBlocking()
            }
            Log.i(
                PLAYBACK_DIAG_TAG,
                "stop unbind+complete totalMs=${android.os.SystemClock.elapsedRealtime() - t0}",
            )
        }
    }

    private fun toggleMute() {
        audioMuted = !audioMuted
        if (audioMuted) {
            volumeBeforeMute = audioVolumePercent.coerceAtLeast(1)
        } else {
            audioVolumePercent = volumeBeforeMute
        }
        applyAudioVolumeFromUi()
        resetControlsTimer()
        persistSettings()
    }

    private fun applyAudioVolumeFromUi() {
        val linear = if (audioMuted) {
            0f
        } else {
            (audioVolumePercent / 100f).coerceIn(0f, 1f)
        }
        previewBackend.setCaptureAudioVolume(linear)
    }

    private fun handlePlayAction() {
        val device = selectedDevice ?: run {
            showMessage(getString(R.string.message_select_device))
            return
        }

        if (selectedFormat == null) {
            showMessage(getString(R.string.message_select_resolution))
            return
        }

        if (!hasRuntimeCameraPermission() || !hasRuntimeRecordAudioPermission()) {
            pendingRuntimePermissionAction = RuntimePermissionAction.START_WATCH
            updatePlayActionInProgress(true)
            requestCameraRecordForWatch.launch(
                arrayOf(Manifest.permission.CAMERA, Manifest.permission.RECORD_AUDIO),
            )
            return
        }

        updatePlayActionInProgress(true)
        lifecycleScope.launch {
            startWatchSession(device)
        }
    }

    private fun requestUsbPermissionForSelection(autoStartAfterGrant: Boolean) {
        val device = selectedDevice ?: run {
            showMessage(getString(R.string.message_select_device))
            return
        }
        if (!hasRuntimeCameraPermission()) {
            startWatchAfterUsbPermission = autoStartAfterGrant
            pendingRuntimePermissionAction = RuntimePermissionAction.REQUEST_USB_PERMISSION
            requestCameraRecordForWatch.launch(
                arrayOf(Manifest.permission.CAMERA, Manifest.permission.RECORD_AUDIO),
            )
            return
        }
        startUsbPermissionRequest(device, autoStartAfterGrant)
    }

    /**
     * USB/UVC work must not run on the main thread — native open, probe sleeps, and bandwidth
     * negotiation can exceed the ~5s input ANR budget (seen as Input dispatching timed out).
     */
    private data class WatchSessionPrep(
        val format: Size?,
        val probedUpdate: List<Size>?,
        val pixelPreference: PixelFormatPreference = PixelFormatPreference.AUTO,
    )

    private fun observeState() {
        lifecycleScope.launch {
            repeatOnLifecycle(Lifecycle.State.STARTED) {
                launch {
                    combine(deviceRepository.devices, captureEngine.state) { devices, state ->
                        devices to state
                    }.collect { (devices, state) ->
                        if (state is CaptureState.Running) {
                            val sessionId = selectedDevice?.id
                            val deviceStillPresent =
                                sessionId != null && devices.any { it.id == sessionId }
                            if (!deviceStillPresent) {
                                Log.i(
                                    PLAYBACK_DIAG_TAG,
                                    "watch session device gone (unplug/refresh) → abandon + async unbind",
                                )
                                previewBackend.prepareForUsbRemoval()
                                replacePreviewTextureAfterUsbRemoval()
                                captureEngine.abandonWatchSessionDueToUsbRemoval()
                                captureEngine.updateReadyState(hasDevices = devices.isNotEmpty())
                                lifecycleScope.launch(Dispatchers.IO) {
                                    previewBackend.unbindPreviewSurface()
                                }
                            }
                        }
                        updateUiForState(devices, captureEngine.state.value)
                    }
                }
                launch {
                    deviceRepository.permissionResults.collect { result ->
                        if (result == UsbCaptureDeviceRepository.PermissionResult.Granted) {
                            cancelPermissionTimeout()
                            lastUsbPermissionGrantedAtMs = android.os.SystemClock.elapsedRealtime()
                            Log.w(
                                PLAYBACK_DIAG_TAG,
                                "USB permission granted; settling ${POST_USB_PERMISSION_SETTLE_MS}ms before probe/play",
                            )
                            delay(POST_USB_PERMISSION_SETTLE_MS)
                            captureEngine.updateReadyState(hasDevices = true)
                            lastResolutionRefreshDeviceId = null
                            refreshResolutions()
                            if (startWatchAfterUsbPermission) {
                                startWatchAfterUsbPermission = false
                                selectedDevice?.let { startWatchSession(it) }
                            }
                        } else {
                            cancelPermissionTimeout()
                            startWatchAfterUsbPermission = false
                            captureEngine.setFailed(getString(R.string.message_permission_denied))
                            updatePlayActionInProgress(false)
                            showMessage(getString(R.string.message_permission_denied))
                            updateStartupActions()
                        }
                    }
                }
            }
        }
    }

    private fun updateUiForState(devices: List<CaptureDevice>, state: CaptureState) {
        devicesUi = devices
        val isRunning = state is CaptureState.Running
        isPlaybackRunningUi = isRunning
        if (isRunning || (isPlayActionInProgress && state is CaptureState.Failed)) {
            updatePlayActionInProgress(false)
        }
        updateSystemUiForPlayback(isRunning)
        if (::previewTexture.isInitialized) {
            previewTexture.isVisible = isRunning
        }
        if (isRunning) {
            cancelAutoStartPlayback()
        }

        if (!isRunning) {
            telemetryJob?.cancel()
            cancelConnectingWatchdog()
            hasRetriedConnectingSession = false
            lowFpsBelowThresholdSinceMs = 0L
            lastTelemetryLogAtMs = 0L
            isLowFpsWarningVisible = false
            statsOverlayText = ""

            if (devices.isEmpty()) {
                cancelAutoStartPlayback()
                selectedDevice = null
                selectedFormat = null
                selectedPixelFormatPreference = PixelFormatPreference.AUTO
                probedFormatSizes = emptyList()
                probedFormatSizesReported = emptyList()
                resolutionDropdownText = ""
                updateStartupActions()
            } else {
                if (selectedDevice == null || devices.none { it.id == selectedDevice!!.id }) {
                    cancelAutoStartPlayback()
                    selectedDevice = devices.first()
                    selectedFormat = null
                    selectedPixelFormatPreference = PixelFormatPreference.AUTO
                    probedFormatSizes = emptyList()
                    probedFormatSizesReported = emptyList()
                    resolutionDropdownText = ""
                    lastResolutionRefreshDeviceId = null
                }
                // Refresh selected device snapshot so updated display labels
                // (e.g., USB capability/speed suffixes) are always reflected.
                selectedDevice = selectedDevice?.let { current ->
                    devices.firstOrNull { it.id == current.id } ?: current
                }
                maybeRefreshResolutionsAfterDeviceOrPermissionChange()
                updateStartupActions()
            }

        } else {
            startTelemetryLoop()
            showControls()
            if (connectingWatchdogJob == null && !previewBackend.hasReceivedFirstVideoFrame()) {
                val dev = selectedDevice
                val fmt = selectedFormat
                if (dev != null && fmt != null) {
                    startConnectingWatchdog(dev, fmt)
                }
            }
            applyAudioVolumeFromUi()
            window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        }

        if (!isRunning) {
            window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        }

        updatePipParams()
        updateConnectingOverlay(isRunning)
    }

    private fun replacePreviewTextureAfterUsbRemoval() {
        if (::previewTexture.isInitialized) {
            previewTexture.surfaceTextureListener = null
            previewTexture.isVisible = false
        }
        previewTextureGeneration++
        updatePreviewScale()
    }

    private fun updateConnectingOverlay(isRunning: Boolean) {
        isConnectingVisible = isRunning && !previewBackend.hasReceivedFirstVideoFrame()
    }

    private fun cancelConnectingWatchdog() {
        connectingWatchdogJob?.cancel()
        connectingWatchdogJob = null
    }

    private fun maybeRefreshResolutionsAfterDeviceOrPermissionChange() {
        val device = selectedDevice ?: return
        val hasUsb = deviceRepository.hasPermission(device)
        if (device.id != lastResolutionRefreshDeviceId || hasUsb != lastResolutionRefreshHadUsbPermission) {
            lastResolutionRefreshDeviceId = device.id
            lastResolutionRefreshHadUsbPermission = hasUsb
            refreshResolutions()
        }
    }

    private fun selectedDeviceCompatibilityIssue(): DeviceCompatibilityIssue? =
        DeviceCompatibilityIssues.issueFor(selectedDevice)

    private fun updateCompatibilityWarning() {
        // Compose reads selectedDeviceCompatibilityIssue() directly.
    }

    private fun updatePlayActionInProgress(inProgress: Boolean) {
        if (isPlayActionInProgress == inProgress) {
            return
        }
        isPlayActionInProgress = inProgress
        updateStartupActions()
    }

    private fun failPlayActionStartup(message: String) {
        updatePlayActionInProgress(false)
        captureEngine.setFailed(message)
        showMessage(message)
    }

    private fun updateStartupActions() {
        val device = selectedDevice
        val hasDevice = device != null
        val hasUsbPermission = device?.let { deviceRepository.hasPermission(it) } == true
        val hasResolution = selectedFormat != null && !isResolutionProbeInProgress
        val needsAccess = hasDevice && (
            !hasUsbPermission ||
                !hasRuntimeCameraPermission() ||
                !hasRuntimeRecordAudioPermission()
            )
        val showUsb2BandwidthWarning = hasDevice && !needsAccess && device?.usbCapabilityLabel == "USB 2"
        val isRequestingPermission = captureEngine.state.value is CaptureState.RequestingPermission

        showPermissionNotice = needsAccess || showUsb2BandwidthWarning
        permissionNoticeText = getString(
            if (showUsb2BandwidthWarning) {
                R.string.startup_usb2_warning
            } else {
                R.string.startup_permission_notice
            },
        )
        showUsbPermissionButton = hasDevice && !hasUsbPermission
        canPlaySelection =
            hasDevice && hasUsbPermission && hasResolution && !isPlayActionInProgress
        if (isRequestingPermission) {
            showUsbPermissionButton = false
        }
    }

    private fun refreshResolutions() {
        val device = selectedDevice ?: run {
            Log.d(RESOLUTION_PROBE_TAG, "refreshResolutions: skip (no selected device)")
            cancelAutoStartPlayback()
            resolutionProbeJob?.cancel()
            resolutionProbeJob = null
            probingResolutionDeviceId = null
            isResolutionProbeInProgress = false
            updateStartupActions()
            return
        }
        if (!deviceRepository.hasPermission(device)) {
            Log.d(
                RESOLUTION_PROBE_TAG,
                "refreshResolutions: skip (no USB permission) id=${device.id} name=${device.name}",
            )
            cancelAutoStartPlayback()
            resolutionProbeJob?.cancel()
            resolutionProbeJob = null
            probingResolutionDeviceId = null
            isResolutionProbeInProgress = false
            resolutionDropdownText = ""
            probedFormatSizes = emptyList()
            probedFormatSizesReported = emptyList()
            selectedFormat = null
            updateStartupActions()
            return
        }

        if (resolutionProbeJob?.isActive == true && probingResolutionDeviceId == device.id) {
            Log.d(
                RESOLUTION_PROBE_TAG,
                "refreshResolutions: skip duplicate in-flight probe id=${device.id}",
            )
            return
        }

        resolutionProbeJob?.cancel()
        cancelAutoStartPlayback()
        resolutionProbeJob = null
        probingResolutionDeviceId = device.id
        isResolutionProbeInProgress = true
        selectedFormat = null
        probedFormatSizes = emptyList()
        probedFormatSizesReported = emptyList()
        updateStartupActions()
        resolutionDropdownText = getString(R.string.state_checking_formats)

        Log.i(
            RESOLUTION_PROBE_TAG,
            "refreshResolutions: scheduling probe id=${device.id} name=${device.name}",
        )
        resolutionProbeJob = lifecycleScope.launch {
            val sizes = kotlinx.coroutines.withContext(kotlinx.coroutines.Dispatchers.IO) {
                previewBackend.probeSupportedSizes(device)
            }
            if (selectedDevice?.id != device.id) {
                Log.i(
                    RESOLUTION_PROBE_TAG,
                    "refreshResolutions: discard stale probe id=${device.id} selected=${selectedDevice?.id}",
                )
                return@launch
            }
            if (probingResolutionDeviceId != device.id) {
                Log.i(
                    RESOLUTION_PROBE_TAG,
                    "refreshResolutions: discard superseded probe id=${device.id} active=${probingResolutionDeviceId}",
                )
                return@launch
            }
            probedFormatSizesReported = sizes
            val effectiveSizes = applyDebugUnsafeFormatSynthesis(sizes)
            probedFormatSizes = effectiveSizes
            if (effectiveSizes.isNotEmpty()) {
                previewBackend.consumeLastProbeOpenFailed()
                val remembered = loadRememberedFormat(device, effectiveSizes, sizes)
                val compatibilityDefault = pickDefaultFormat(effectiveSizes, selectedDeviceCompatibilityIssue(), sizes)
                val defaultSeed = remembered?.first
                        ?: compatibilityDefault
                        ?: effectiveSizes.first().let { Size(it) }
                selectedPixelFormatPreference = remembered?.second ?: PixelFormatPreference.AUTO
                val defaultFormat = remembered?.first ?: (
                    resolveFormatChoiceForPreference(
                        effectiveSizes,
                        defaultSeed.width,
                        defaultSeed.height,
                        selectedPixelFormatPreference,
                        requestedFps = try {
                            defaultSeed.getCurrentFrameRate()
                        } catch (_: Exception) {
                            null
                        },
                        reportedProbeSizes = sizes,
                    ) ?: Size(defaultSeed)
                    )
                val defaultSource = when {
                    remembered != null -> "remembered"
                    compatibilityDefault != null -> "compatibility_default"
                    else -> "first_reported"
                }
                selectedFormat = defaultFormat
                val label = formatResolutionLabel(defaultFormat, selectedPixelFormatPreference)
                resolutionDropdownText = label
                updateStartupActions()
                Log.i(
                    RESOLUTION_PROBE_TAG,
                    "refreshResolutions: UI updated size=${effectiveSizes.size} source=$defaultSource " +
                        "pref=${selectedPixelFormatPreference.prefValue} defaultLabel=$label",
                )
            } else {
                val probeOpenFailed = previewBackend.consumeLastProbeOpenFailed()
                selectedFormat = null
                probedFormatSizesReported = emptyList()
                resolutionDropdownText = getString(
                    if (probeOpenFailed) {
                        R.string.state_probe_unavailable
                    } else {
                        R.string.state_no_formats
                    },
                )
                if (probeOpenFailed) {
                    showMessage(getString(R.string.message_uvc_open_failed))
                }
                updateStartupActions()
                Log.w(
                    RESOLUTION_PROBE_TAG,
                    "refreshResolutions: probe returned empty list id=${device.id} name=${device.name} " +
                        "probeOpenFailed=$probeOpenFailed",
                )
            }
        }.also { job ->
            job.invokeOnCompletion {
                runOnUiThread {
                    if (resolutionProbeJob != job) {
                        return@runOnUiThread
                    }
                    resolutionProbeJob = null
                    probingResolutionDeviceId = null
                    isResolutionProbeInProgress = false
                    updateStartupActions()
                    if (selectedDevice?.id == device.id && selectedFormat != null) {
                        scheduleAutoStartPlaybackAfterResolution(device)
                    }
                }
            }
        }
    }

    private fun scheduleAutoStartPlaybackAfterResolution(device: CaptureDevice) {
        if (AUTO_START_PLAYBACK != 1) {
            return
        }
        autoStartPlaybackJob?.cancel()
        autoStartPlaybackJob = lifecycleScope.launch {
            Log.w(
                PLAYBACK_DIAG_TAG,
                "debug AUTO_START_PLAYBACK armed; waiting ${AUTO_START_PLAYBACK_DELAY_MS}ms",
            )
            delay(AUTO_START_PLAYBACK_DELAY_MS)
            if (AUTO_START_PLAYBACK != 1) {
                return@launch
            }
            if (captureEngine.state.value is CaptureState.Running) {
                return@launch
            }
            if (
                selectedDevice?.id != device.id ||
                selectedFormat == null ||
                isResolutionProbeInProgress ||
                !deviceRepository.hasPermission(device)
            ) {
                Log.w(
                    PLAYBACK_DIAG_TAG,
                    "debug AUTO_START_PLAYBACK skipped; startup selection changed or is not ready",
                )
                return@launch
            }
            Log.w(PLAYBACK_DIAG_TAG, "debug AUTO_START_PLAYBACK invoking Play")
            handlePlayAction()
        }
    }

    private fun cancelAutoStartPlayback() {
        autoStartPlaybackJob?.cancel()
        autoStartPlaybackJob = null
    }

    private data class ResolutionChoice(
        val width: Int,
        val height: Int,
        val fps: Float,
        val pixelPreference: PixelFormatPreference,
        val unsafeDebug: Boolean,
    )

    private fun resolutionChoices(): List<ResolutionChoice> {
        if (probedFormatSizes.isEmpty()) {
            return emptyList()
        }
        val choices = mutableListOf<ResolutionChoice>()
        val resolutionGroups = groupSizesByResolution(probedFormatSizes)
        for (group in resolutionGroups) {
            for (fps in group.fpsOptions) {
                val formatOptions = supportedFormatPreferencesForResolutionAndFps(
                    probedFormatSizes,
                    group.width,
                    group.height,
                    fps,
                )
                if (formatOptions.isEmpty()) {
                    continue
                }
                for (formatPreference in formatOptions) {
                    val unsafeChoice = isDebugUnsafeFormatChoice(
                        probedFormatSizesReported,
                        group.width,
                        group.height,
                        fps,
                        formatPreference,
                    )
                    choices += ResolutionChoice(
                        group.width,
                        group.height,
                        fps,
                        formatPreference,
                        unsafeChoice,
                    )
                }
            }
        }
        return choices
    }

    private fun selectResolutionChoice(choice: ResolutionChoice) {
        val resolved = resolveFormatChoiceForPreference(
            probedFormatSizes,
            choice.width,
            choice.height,
            choice.pixelPreference,
            requestedFps = choice.fps,
            reportedProbeSizes = probedFormatSizesReported.takeIf {
                choice.pixelPreference == PixelFormatPreference.AUTO
            },
        ) ?: return
        selectedPixelFormatPreference = choice.pixelPreference
        selectedFormat = resolved
        selectedDevice?.let { device ->
            persistFormatForDevice(device, selectedFormat!!, choice.pixelPreference)
        }
        resolutionDropdownText = formatResolutionLabel(selectedFormat!!, choice.pixelPreference)
        updateStartupActions()
    }

    private fun showDeviceMenu(anchor: View) {
        val popup = AppCompatPopupMenu(popupMenuContext(), anchor)
        val menu = popup.menu
        if (devicesUi.isEmpty()) {
            menu.add(Menu.NONE, Menu.NONE, Menu.NONE, getString(R.string.hint_no_capture_devices))
                .isEnabled = false
        } else {
            devicesUi.forEachIndexed { index, device ->
                menu.add(Menu.NONE, index, Menu.NONE, device.displayName)
            }
        }
        popup.setOnMenuItemClickListener { item ->
            val device = devicesUi.getOrNull(item.itemId) ?: return@setOnMenuItemClickListener false
            selectDevice(device)
            true
        }
        popup.show()
    }

    private fun showResolutionFormatMenu(anchor: View) {
        if (probedFormatSizes.isEmpty()) {
            refreshResolutions()
            return
        }
        val popup = AppCompatPopupMenu(popupMenuContext(), anchor)
        val menu = popup.menu
        addMenuHeaderWithDivider(menu, getString(R.string.resolution_menu_resolution_header))
        val choiceIds = mutableMapOf<Int, ResolutionChoice>()
        var nextId = MENU_ID_RESOLUTION_BASE
        var addedOtherResolutionsHeader = false
        for (group in groupSizesByResolution(probedFormatSizes)) {
            val isStandard = isStandardResolution(group.width, group.height)
            if (!isStandard && !addedOtherResolutionsHeader) {
                addMenuHeaderWithDivider(menu, getString(R.string.menu_other_resolutions_header))
                addedOtherResolutionsHeader = true
            }
            val resolutionSub: Menu =
                menu.addSubMenu(Menu.NONE, Menu.NONE, Menu.NONE, "${group.width}x${group.height}")
            addMenuHeaderWithDivider(resolutionSub, getString(R.string.resolution_menu_frame_rate_header))
            for (fps in group.fpsOptions) {
                val formatOptions = supportedFormatPreferencesForResolutionAndFps(
                    probedFormatSizes,
                    group.width,
                    group.height,
                    fps,
                )
                if (formatOptions.isEmpty()) {
                    continue
                }
                val fpsSub: Menu = resolutionSub.addSubMenu(
                    Menu.NONE,
                    Menu.NONE,
                    Menu.NONE,
                    getString(R.string.resolution_menu_fps_item, fps.roundToInt()),
                )
                addMenuHeaderWithDivider(fpsSub, getString(R.string.resolution_menu_pixel_format_header))
                for (formatPreference in formatOptions) {
                    val unsafeChoice = isDebugUnsafeFormatChoice(
                        probedFormatSizesReported,
                        group.width,
                        group.height,
                        fps,
                        formatPreference,
                    )
                    val id = nextId++
                    choiceIds[id] = ResolutionChoice(
                        group.width,
                        group.height,
                        fps,
                        formatPreference,
                        unsafeChoice,
                    )
                    fpsSub.add(Menu.NONE, id, Menu.NONE, formatMenuLabel(formatPreference, unsafeChoice))
                }
            }
        }
        popup.setOnMenuItemClickListener { item ->
            val choice = choiceIds[item.itemId] ?: return@setOnMenuItemClickListener false
            selectResolutionChoice(choice)
            true
        }
        popup.show()
    }

    private fun popupMenuContext(): Context =
        ContextThemeWrapper(this, R.style.ThemeOverlay_Consolation_PopupMenu)

    private fun updateAspectRatio(width: Int, height: Int) {
        if (width > 0 && height > 0) {
            previewAspectRatio = width.toFloat() / height.toFloat()
        }
    }

    private fun startTelemetryLoop() {
        telemetryJob?.cancel()
        telemetryJob = lifecycleScope.launch {
            while (true) {
                if (
                    captureEngine.state.value is CaptureState.Running &&
                    !previewBackend.hasReceivedFirstVideoFrame() &&
                    previewBackend.consumeLastPreviewStartFailed()
                ) {
                    failConnectingSession(getString(R.string.message_uvc_open_failed))
                    return@launch
                }
                val stats = previewBackend.getTelemetry()
                updateConnectingOverlay(captureEngine.state.value is CaptureState.Running)
                if (previewBackend.hasReceivedFirstVideoFrame()) {
                    cancelConnectingWatchdog()
                }
                updateStatsOverlay(stats)
                logTelemetryLine(stats)
                updateLowFpsWarning(stats)
                delay(500)
            }
        }
    }

    private fun updateStatsOverlay(stats: org.centennialoss.consolation.core.telemetry.TelemetrySnapshot) {
        statsOverlayText =
            if (isStatsVisible && statsPosition != StatsPosition.OFF && isPlaybackRunningUi) {
                buildTelemetryOverlayCompactText(stats)
            } else {
                ""
            }
    }

    private fun buildTelemetryOverlayCompactText(stats: org.centennialoss.consolation.core.telemetry.TelemetrySnapshot): String {
        val base = listOf(
            "${stats.width}x${stats.height}/${stats.configuredFps}",
            "${stats.pixelFormat}",
            "Fps:${stats.fps}",
            "Lag: ${format0(stats.nativeEndToEndLatencyAvgMs)}ms",
        )
        if (!isDebugStatsEnabled) {
            return base.joinToString(" | ")
        }
        val payload = formatTelemetryPayloadLabel(stats.nativePayloadAvgKb)
        val intervalMs = stats.nativeFrameInterval100ns / 10_000.0
        val usbLabel = if (stats.nativeIsIsochronous) {
            "USB: Iso ${stats.nativeAltSetting}"
        } else {
            "USB: Bulk"
        }
        return (base + listOf(
            "FDrop:${stats.droppedFrames}",
            "Evct:${format0(stats.nativeQueueEnqAvgFrames)}",
            "Cb:${format0(stats.nativeUvcCbAvgMs)}",
            "CbLg:${format0(stats.nativeCbLagAvgMs)}",
            "CbSkp:${stats.nativePreCbSkip}",
            "SDrop:${stats.nativeStreamDrop}",
            "Cnv:${format0(stats.nativePreviewConvAvgMs)}",
            "Cad:${format1(intervalMs)}",
            usbLabel,
            "${payload}",
        )).joinToString(" | ")
    }

    private fun buildTelemetryLogText(stats: org.centennialoss.consolation.core.telemetry.TelemetrySnapshot): String {
        val payload = formatTelemetryPayloadLabel(stats.nativePayloadAvgKb)
        return listOf(
            "Res:${stats.width}x${stats.height}/${stats.configuredFps}",
            "Fmt:${stats.pixelFormat}",
            "Fps:${stats.fps}",
            "Drop:${stats.droppedFrames}",
            "QD:${format0(stats.nativeQueuedAvgFrames)}",
            "QE:${format0(stats.nativeQueueEnqAvgFrames)}",
            "CbMs:${format1(stats.nativeUvcCbAvgMs)}",
            "CbMax:${format1(stats.nativeUvcCbMaxMs)}",
            "LagMs:${format1(stats.nativeCbLagAvgMs)}",
            "LagMax:${format1(stats.nativeCbLagMaxMs)}",
            "LagCnt:${stats.nativeCbLagCount}",
            "Pub/s:${format0(stats.nativePubFps)}",
            "PreSk:${stats.nativePreCbSkip}",
            "SDrop:${stats.nativeStreamDrop}",
            "Intv:${stats.nativeFrameInterval100ns}",
            "Alt:${stats.nativeAltSetting}",
            "CvMs:${format1(stats.nativePreviewConvAvgMs)}",
            "E2EMs:${format1(stats.nativeEndToEndLatencyAvgMs)}",
            "E2EMx:${format1(stats.nativeEndToEndMaxMs)}",
            "Pay:${payload}",
        ).joinToString(" | ")
    }

    private fun logTelemetryLine(stats: org.centennialoss.consolation.core.telemetry.TelemetrySnapshot) {
        val now = android.os.SystemClock.elapsedRealtime()
        if (now - lastTelemetryLogAtMs < 1_000L) {
            return
        }
        lastTelemetryLogAtMs = now
        Log.i(TELEMETRY_LOG_TAG, buildTelemetryLogText(stats))
    }

    /** Average native payload size for overlay: KiB until 1024 KiB, then MiB. */
    private fun formatTelemetryPayloadLabel(avgKb: Double): String {
        if (avgKb <= 0.0 || avgKb.isNaN()) {
            return "0 KB"
        }
        return if (avgKb >= 1024.0) {
            String.format(Locale.US, "%.2f MB", avgKb / 1024.0)
        } else {
            String.format(Locale.US, "%.0f KB", avgKb)
        }
    }

    private fun format1(value: Double): String = String.format(Locale.US, "%.1f", value)
    private fun format0(value: Double): String = String.format(Locale.US, "%.0f", value)

    private fun updateLowFpsWarning(stats: org.centennialoss.consolation.core.telemetry.TelemetrySnapshot) {
        if (!previewBackend.hasReceivedFirstVideoFrame()) {
            lowFpsBelowThresholdSinceMs = 0L
            isLowFpsWarningVisible = false
            return
        }
        val configured = stats.configuredFps
        val actual = stats.fps
        val deltaOk = configured > 0 &&
            actual < configured &&
            (configured - actual) >= LOW_FPS_MIN_DELTA

        val now = android.os.SystemClock.elapsedRealtime()
        if (!isLowFpsWarningEnabled || !deltaOk) {
            lowFpsBelowThresholdSinceMs = 0L
            isLowFpsWarningVisible = false
            return
        }
        if (lowFpsBelowThresholdSinceMs == 0L) {
            lowFpsBelowThresholdSinceMs = now
        }
        val sustained = now - lowFpsBelowThresholdSinceMs >= LOW_FPS_SUSTAIN_MS
        val showWarning = sustained
        isLowFpsWarningVisible = showWarning
    }

    private fun showControls() {
        if (captureEngine.state.value is CaptureState.Running) {
            updateSystemUiForPlayback(isPlaybackActive = true)
        }
        areControlsVisible = true
        resetControlsTimer()
    }

    private fun resetControlsTimer() {
        controlsAutoHideJob?.cancel()
        controlsAutoHideJob = lifecycleScope.launch {
            delay(3000)
            areControlsVisible = false
        }
    }

    private fun updatePreviewScale() {
        // Compose applies scale/rotation from state.
    }

    private fun showSettingsDialog() {
        activeSheet = ActiveSheet.SETTINGS
    }

    private fun showHelpDialog() {
        activeSheet = ActiveSheet.HELP
    }

    private fun showAboutDialog() {
        activeSheet = ActiveSheet.ABOUT
    }

    private fun showLowFpsInfo() {
        activeSheet = ActiveSheet.LOW_FPS
    }

    private fun showCompatibilityIssueDialog(issue: DeviceCompatibilityIssue) {
        activeSheet = ActiveSheet.COMPATIBILITY
    }

    private fun showCaptureAudioFailureDialog() {
        if (isFinishing || isDestroyed) {
            return
        }
        activeSheet = ActiveSheet.AUDIO_FAILURE
    }

    private fun showMessageDialog(title: String, message: String) {
        showMessage("$title: $message")
    }

    private fun persistFormatForDevice(
        device: CaptureDevice,
        format: Size,
        pixelPreference: PixelFormatPreference,
    ) {
        val fps = try {
            format.getCurrentFrameRate().roundToInt().coerceAtLeast(1)
        } catch (_: Exception) {
            return
        }
        prefs.edit()
            .putString(
                formatPreferenceKey(device),
                "${format.width},${format.height},$fps,${pixelPreference.prefValue}",
            )
            .apply()
    }

    private fun loadRememberedFormat(
        device: CaptureDevice,
        sizes: List<Size>,
        reportedProbeSizes: List<Size>,
    ): Pair<Size, PixelFormatPreference>? {
        val parts = prefs.getString(formatPreferenceKey(device), null)
            ?.split(',')
            ?: return null
        if (parts.size < 3) return null
        val width = parts[0].toIntOrNull() ?: return null
        val height = parts[1].toIntOrNull() ?: return null
        val fps = parts[2].toFloatOrNull() ?: return null
        val pixelPreference = parts.getOrNull(3)?.let { pref ->
            PixelFormatPreference.entries.firstOrNull { it.prefValue == pref }
        } ?: PixelFormatPreference.AUTO
        if (sizes.none { it.width == width && it.height == height }) return null
        val resolved =
            resolveFormatChoiceForPreference(
                sizes,
                width,
                height,
                pixelPreference,
                requestedFps = fps,
                reportedProbeSizes = reportedProbeSizes.takeIf {
                    pixelPreference == PixelFormatPreference.AUTO
                },
            ) ?: return null
        return resolved to pixelPreference
    }

    private fun formatPreferenceKey(device: CaptureDevice): String {
        return "$KEY_DEVICE_FORMAT_PREFIX${device.vendorId}:${device.productId}:${device.name}"
    }

    private suspend fun startWatchSession(device: CaptureDevice) {
        try {
            if (!deviceRepository.hasPermission(device)) {
                startUsbPermissionRequest(device, autoStartAfterGrant = true)
                return
            }
            waitForRecentUsbPermissionSettle()

            val snapshotFormat = selectedFormat
            val snapshotProbedEmpty = probedFormatSizes.isEmpty()

            val prep = withContext(Dispatchers.IO) {
                previewBackend.configureSession(device)

                var format = snapshotFormat
                var newProbed: List<Size>? = null
                var pixelPreference = selectedPixelFormatPreference
                if (format == null || snapshotProbedEmpty) {
                    newProbed = previewBackend.probeSupportedSizes(device)
                    val remembered = loadRememberedFormat(device, newProbed, newProbed)
                    val seed =
                        remembered?.first
                            ?: pickDefaultFormat(newProbed, DeviceCompatibilityIssues.issueFor(device), newProbed)
                            ?: newProbed.firstOrNull()?.let { Size(it) }
                    pixelPreference = remembered?.second ?: PixelFormatPreference.AUTO
                    format = seed?.let {
                        resolveFormatChoiceForPreference(
                            newProbed,
                            it.width,
                            it.height,
                            pixelPreference,
                            requestedFps = try {
                                it.getCurrentFrameRate()
                            } catch (_: Exception) {
                                null
                            },
                            reportedProbeSizes = newProbed,
                        )
                    } ?: seed
                }
                if (format == null) {
                    return@withContext WatchSessionPrep(null, newProbed ?: emptyList(), pixelPreference)
                }

                val fps = try {
                    format.getCurrentFrameRate().roundToInt().coerceAtLeast(1)
                } catch (_: Exception) {
                    30
                }
                // Keep runtime request aligned with the already-resolved concrete Size so AUTO
                // never asks backend for a format unavailable at the selected resolution/fps.
                val runtimeFrameFormat = when (pixelPreference) {
                    PixelFormatPreference.AUTO -> format.frame_type
                    else -> pixelPreference.frameFormat ?: format.frame_type
                }
                previewBackend.setPreferredPixelFormat(runtimeFrameFormat)
                previewBackend.setPreviewSize(format.width, format.height, fps)

                WatchSessionPrep(format, newProbed, pixelPreference)
            }

            if (prep.format == null) {
                prep.probedUpdate?.let {
                    probedFormatSizesReported = it
                    probedFormatSizes = applyDebugUnsafeFormatSynthesis(it)
                }
                selectedFormat = null
                updatePlayActionInProgress(false)
                showMessage(getString(R.string.state_no_formats))
                return
            }

            prep.probedUpdate?.let {
                probedFormatSizesReported = it
                probedFormatSizes = applyDebugUnsafeFormatSynthesis(it)
            }
            selectedFormat = prep.format
            selectedPixelFormatPreference = prep.pixelPreference
            persistFormatForDevice(device, prep.format, selectedPixelFormatPreference)

            updateAspectRatio(prep.format.width, prep.format.height)
            hasRetriedConnectingSession = false
            captureEngine.startWatching(device)
            isPlaybackRunningUi = true
            areControlsVisible = true
            if (::previewTexture.isInitialized) {
                previewTexture.isVisible = true
                previewTexture.doOnLayout {
                    try {
                        previewBackend.bindPreviewSurface(previewTexture)
                    } catch (e: Exception) {
                        Log.e(PLAYBACK_DIAG_TAG, "bindPreviewSurface failed", e)
                        failConnectingSession(getString(R.string.message_uvc_open_failed))
                    }
                }
            }
            startConnectingWatchdog(device, prep.format)
        } catch (e: kotlinx.coroutines.CancellationException) {
            if (captureEngine.state.value !is CaptureState.Running) {
                updatePlayActionInProgress(false)
            }
            throw e
        } catch (e: Exception) {
            Log.e(PLAYBACK_DIAG_TAG, "startWatchSession failed", e)
            failPlayActionStartup(getString(R.string.message_uvc_open_failed))
        }
    }

    private fun startConnectingWatchdog(device: CaptureDevice, format: Size) {
        cancelConnectingWatchdog()
        connectingWatchdogJob = lifecycleScope.launch {
            delay(CONNECTING_RETRY_TIMEOUT_MS)
            if (captureEngine.state.value !is CaptureState.Running) return@launch
            if (previewBackend.hasReceivedFirstVideoFrame()) return@launch
            if (previewBackend.consumeLastPreviewStartFailed()) {
                failConnectingSession(getString(R.string.message_uvc_open_failed))
                return@launch
            }
            Log.w(
                PLAYBACK_DIAG_TAG,
                "connecting watchdog: no first frame after ${CONNECTING_RETRY_TIMEOUT_MS}ms; failing session " +
                    "for selected mode ${format.width}x${format.height}",
            )
            failConnectingSession(getString(R.string.message_uvc_no_frames_after_start))
        }
    }

    private fun failConnectingSession(message: String) {
        cancelConnectingWatchdog()
        hasRetriedConnectingSession = false
        updatePlayActionInProgress(false)
        lifecycleScope.launch(Dispatchers.IO) {
            // Force a hard native reset before returning to startup so the next Play attempt
            // does not block in stopUvcStreamingBlocking waiting for a wedged teardown.
            previewBackend.prepareForUsbRemoval()
            previewBackend.unbindPreviewSurface()
        }
        captureEngine.setFailed(message)
        showBlockingErrorDialog(message)
    }

    private suspend fun waitForRecentUsbPermissionSettle() {
        val remaining = lastUsbPermissionGrantedAtMs +
            POST_USB_PERMISSION_SETTLE_MS -
            android.os.SystemClock.elapsedRealtime()
        if (remaining > 0) {
            Log.w(
                PLAYBACK_DIAG_TAG,
                "startWatchSession: waiting ${remaining}ms for recent USB permission settle",
            )
            delay(remaining)
        }
    }

    private fun startUsbPermissionRequest(device: CaptureDevice, autoStartAfterGrant: Boolean) {
        val didStartRequest = deviceRepository.requestPermission(device)
        if (!didStartRequest) {
            startWatchAfterUsbPermission = false
            captureEngine.setFailed(getString(R.string.message_permission_request_start_failed))
            updatePlayActionInProgress(false)
            showMessage(getString(R.string.message_permission_request_start_failed))
            return
        }
        startWatchAfterUsbPermission = autoStartAfterGrant
        captureEngine.setRequestingPermission()
        startPermissionRequestTimeout()
    }

    private fun startPermissionRequestTimeout() {
        cancelPermissionTimeout()
        permissionTimeoutJob = lifecycleScope.launch {
            delay(10000)
            if (captureEngine.state.value is CaptureState.RequestingPermission) {
                captureEngine.setFailed(getString(R.string.message_permission_request_timeout))
                updatePlayActionInProgress(false)
                showMessage(getString(R.string.message_permission_request_timeout))
            }
        }
    }

    private fun cancelPermissionTimeout() {
        permissionTimeoutJob?.cancel()
        permissionTimeoutJob = null
    }

    private fun hasRuntimeCameraPermission(): Boolean =
        ContextCompat.checkSelfPermission(this, Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED

    private fun hasRuntimeRecordAudioPermission(): Boolean =
        ContextCompat.checkSelfPermission(this, Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED

    private fun showMessage(message: String) {
        Snackbar.make(rootView, message, Snackbar.LENGTH_SHORT).show()
    }

    private fun showBlockingErrorDialog(message: String) {
        if (isFinishing || isDestroyed) return
        blockingErrorMessage = message
    }

    private fun loadSettingsFromPrefs() {
        statsPosition = when (prefs.getInt(KEY_STATS_POSITION, StatsPosition.OFF.ordinal)) {
            StatsPosition.OFF.ordinal -> StatsPosition.OFF
            StatsPosition.BOTTOM_RIGHT.ordinal -> StatsPosition.BOTTOM_RIGHT
            else -> StatsPosition.BOTTOM_LEFT
        }
        isStatsVisible = prefs.getBoolean(KEY_STATS_VISIBLE, false)
        if (!isStatsVisible) {
            statsPosition = StatsPosition.OFF
        }
        isLowFpsWarningEnabled = prefs.getBoolean(KEY_LOW_FPS_WARN, true)
        isDebugStatsEnabled = prefs.getBoolean(KEY_SHOW_DEBUG_STATS, false)
        isPipEnabled = prefs.getBoolean(KEY_PIP_ENABLED, true)
        currentRotation = prefs.getInt(KEY_ROTATION, 0)
        isFlippedHorizontal = prefs.getBoolean(KEY_FLIP_H, false)
        isFlippedVertical = prefs.getBoolean(KEY_FLIP_V, false)
        currentZoom = prefs.getInt(KEY_ZOOM, 0).coerceIn(0, 100)
        audioVolumePercent = prefs.getInt(KEY_VOLUME, 100).coerceIn(0, 100)
        audioMuted = prefs.getBoolean(KEY_MUTED, false)
    }

    private fun persistSettings() {
        prefs.edit()
            .putInt(KEY_STATS_POSITION, statsPosition.ordinal)
            .putBoolean(KEY_STATS_VISIBLE, isStatsVisible)
            .putBoolean(KEY_LOW_FPS_WARN, isLowFpsWarningEnabled)
            .putBoolean(KEY_SHOW_DEBUG_STATS, isDebugStatsEnabled)
            .putBoolean(KEY_PIP_ENABLED, isPipEnabled)
            .putInt(KEY_ROTATION, currentRotation)
            .putBoolean(KEY_FLIP_H, isFlippedHorizontal)
            .putBoolean(KEY_FLIP_V, isFlippedVertical)
            .putInt(KEY_ZOOM, currentZoom)
            .putInt(KEY_VOLUME, audioVolumePercent)
            .putBoolean(KEY_MUTED, audioMuted)
            .apply()
    }

    @Composable
    private fun MainScreen() {
        val maxScale = 1.175f * 1.5f
        val baseScale = 1.0f + (currentZoom / 100.0f) * (maxScale - 1.0f)
        Box(
            modifier = Modifier
                .fillMaxSize()
                .background(Color.Black)
                .clickable(
                    indication = null,
                    interactionSource = remember { MutableInteractionSource() },
                ) {
                    if (captureEngine.state.value is CaptureState.Running) {
                        showControls()
                    }
                },
        ) {
            Box(modifier = Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                key(previewTextureGeneration) {
                    AndroidView(
                        factory = { context ->
                            TextureView(context).also {
                                previewTexture = it
                                it.isVisible = isPlaybackRunningUi
                            }
                        },
                        modifier = Modifier
                            .fillMaxHeight()
                            .aspectRatio(previewAspectRatio)
                            .graphicsLayer {
                                scaleX = baseScale * if (isFlippedHorizontal) -1f else 1f
                                scaleY = baseScale * if (isFlippedVertical) -1f else 1f
                                rotationZ = currentRotation.toFloat()
                            },
                        update = {
                            it.isVisible = isPlaybackRunningUi
                        },
                    )
                }
            }

            if (!isPlaybackRunningUi) {
                StartupScreen()
            }

            if (isConnectingVisible) {
                Text(
                    text = getString(R.string.connecting_capture_card),
                    color = Color(0xFF808080),
                    fontSize = 32.sp,
                    modifier = Modifier.align(Alignment.Center),
                )
            }

            if (statsOverlayText.isNotEmpty()) {
                Text(
                    text = statsOverlayText,
                    color = Color.White,
                    fontSize = 12.sp,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                    modifier = Modifier
                        .align(
                            if (statsPosition == StatsPosition.BOTTOM_RIGHT) {
                                Alignment.BottomEnd
                            } else {
                                Alignment.BottomStart
                            },
                        )
                        .padding(16.dp)
                        .background(Color(0x99000000), RoundedCornerShape(4.dp))
                        .padding(6.dp),
                )
            }

            if (isLowFpsWarningVisible) {
                val alignRight = isStatsVisible && statsPosition == StatsPosition.BOTTOM_LEFT
                Text(
                    text = getString(R.string.low_fps_warning),
                    color = Color.White,
                    fontSize = 12.sp,
                    modifier = Modifier
                        .align(if (alignRight) Alignment.BottomEnd else Alignment.BottomStart)
                        .padding(16.dp)
                        .background(Color(0x80FF0000), RoundedCornerShape(2.dp))
                        .clickable { showLowFpsInfo() }
                        .padding(8.dp),
                )
            }

            if (isPlaybackRunningUi && areControlsVisible) {
                PlaybackControlsBar(
                    modifier = Modifier
                        .align(Alignment.BottomCenter)
                        .padding(bottom = 32.dp),
                )
            }

            activeSheet?.let { sheet ->
                ModalBackdrop {
                    when (sheet) {
                        ActiveSheet.HELP -> HelpSheet()
                        ActiveSheet.ABOUT -> AboutSheet()
                        ActiveSheet.SETTINGS -> SettingsSheet()
                        ActiveSheet.COMPATIBILITY -> CompatibilitySheet()
                        ActiveSheet.LOW_FPS -> MessageSheet(
                            title = getString(R.string.low_fps_info_title),
                            message = getString(R.string.low_fps_info_message),
                        )
                        ActiveSheet.AUDIO_FAILURE -> MessageSheet(
                            title = getString(R.string.audio_playback_failed_title),
                            message = getString(R.string.audio_playback_failed_message),
                        )
                    }
                }
            }

            blockingErrorMessage?.let { message ->
                ModalBackdrop {
                    MessageSheet(
                        title = getString(R.string.error_dialog_title),
                        message = message,
                        closeText = getString(android.R.string.ok),
                        onClose = { blockingErrorMessage = null },
                    )
                }
            }
        }
    }

    @Composable
    private fun StartupScreen() {
        BoxWithConstraints(
            modifier = Modifier
                .fillMaxSize()
                .background(
                    Brush.verticalGradient(
                        listOf(Color(0xFF8C0573), Color(0xFF470330), Color.Black),
                    ),
                ),
        ) {
            val phone = maxHeight < 420.dp || maxWidth < 720.dp
            val panelPadding = if (phone) 16.dp else 28.dp
            val iconSize = if (phone) 44.dp else 64.dp
            val titleSize = if (phone) 24.sp else 28.sp
            val labelSize = if (phone) 15.sp else 16.sp
            val sectionGap = 12.dp
            val playSize = if (phone) 56.dp else 72.dp
            val hasUsbPermission = selectedDevice?.let { deviceRepository.hasPermission(it) } == true

            Column(
                modifier = Modifier
                    .align(Alignment.Center)
                    .widthIn(max = if (phone) 480.dp else 660.dp)
                    .fillMaxWidth(if (phone) 0.86f else 1f)
                    .background(Color(0x40100018), RoundedCornerShape(18.dp))
                    .border(1.dp, Color(0x55FFFFFF), RoundedCornerShape(18.dp))
                    .padding(panelPadding),
            ) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Image(
                        painter = painterResource(R.drawable.app_icon_large),
                        contentDescription = getString(R.string.app_name),
                        contentScale = ContentScale.Crop,
                        modifier = Modifier
                            .size(iconSize)
                            .clip(RoundedCornerShape(if (phone) 10.dp else 16.dp)),
                    )
                    Spacer(Modifier.width(12.dp))
                    Text(
                        text = getString(R.string.label_title),
                        color = Color.White,
                        fontSize = titleSize,
                        fontWeight = FontWeight.Bold,
                    )
                    Spacer(Modifier.width(8.dp))
                    Text(
                        text = getString(R.string.label_app_version, AppBuildInfo.version),
                        color = Color(0xB3FFFFFF),
                        fontSize = if (phone) 13.sp else 16.sp,
                    )
                }
                DividerLine(Modifier.padding(top = if (phone) 10.dp else 16.dp, bottom = 0.dp))

                if (showPermissionNotice) {
                    Text(
                        text = permissionNoticeText,
                        textAlign = androidx.compose.ui.text.style.TextAlign.Center,
                        color = if (selectedDevice?.usbCapabilityLabel == "USB 2") {
                            Color(0xFFFFE082)
                        } else {
                            Color(0xCCFFFFFF)
                        },
                        fontSize = if (phone) 13.sp else 15.sp,
                        fontWeight = FontWeight.Bold,
                        lineHeight = if (phone) 17.sp else 21.sp,
                        modifier = Modifier
                            .fillMaxWidth()
                            .padding(top = sectionGap),
                    )
                }

                StartupSelectorRow(
                    label = getString(R.string.label_device),
                    labelSize = labelSize,
                    modifier = Modifier.padding(top = sectionGap),
                ) {
                    DeviceDropdown()
                }

                if (hasUsbPermission) {
                    StartupSelectorRow(
                        label = getString(R.string.label_resolution),
                        labelSize = labelSize,
                        modifier = Modifier.padding(top = if (phone) 12.dp else 32.dp),
                    ) {
                        ResolutionDropdown()
                    }
                }

                if (showUsbPermissionButton) {
                    OutlinePillButton(
                        text = getString(R.string.action_request_permission),
                        modifier = Modifier
                            .fillMaxWidth()
                            .padding(top = 8.dp),
                    ) {
                        requestUsbPermissionForSelection(autoStartAfterGrant = false)
                    }
                }

                Button(
                    onClick = { handlePlayAction() },
                    enabled = canPlaySelection,
                    shape = CircleShape,
                    colors = ButtonDefaults.buttonColors(
                        containerColor = Color(0x55FFFFFF),
                        disabledContainerColor = Color(0x22FFFFFF),
                    ),
                    contentPadding = androidx.compose.foundation.layout.PaddingValues(0.dp),
                    modifier = Modifier
                        .align(Alignment.CenterHorizontally)
                        .padding(top = if (phone) 14.dp else 36.dp)
                        .size(playSize),
                ) {
                    Icon(
                        painter = painterResource(R.drawable.ic_play_arrow),
                        contentDescription = getString(R.string.action_start_video),
                        tint = Color.White.copy(alpha = if (canPlaySelection) 1f else 0.4f),
                        modifier = Modifier.size(if (phone) 30.dp else 42.dp),
                    )
                }
            }

            Row(
                horizontalArrangement = Arrangement.spacedBy(16.dp),
                modifier = Modifier
                    .align(Alignment.BottomEnd)
                    .padding(16.dp),
            ) {
                TextIconButton(getString(R.string.action_settings), R.drawable.ic_settings) { showSettingsDialog() }
                TextIconButton(getString(R.string.action_help), R.drawable.ic_help) { showHelpDialog() }
                TextIconButton(getString(R.string.action_about), R.drawable.ic_info) { showAboutDialog() }
            }
        }
    }

    @Composable
    private fun FieldLabel(text: String, fontSize: androidx.compose.ui.unit.TextUnit, modifier: Modifier = Modifier) {
        Text(
            text = text,
            color = Color.White,
            fontSize = fontSize,
            fontWeight = FontWeight.Bold,
            textAlign = androidx.compose.ui.text.style.TextAlign.End,
            modifier = modifier,
        )
    }

    @Composable
    private fun StartupSelectorRow(
        label: String,
        labelSize: androidx.compose.ui.unit.TextUnit,
        modifier: Modifier = Modifier,
        content: @Composable RowScope.() -> Unit,
    ) {
        Row(
            verticalAlignment = Alignment.CenterVertically,
            modifier = modifier.fillMaxWidth(),
        ) {
            FieldLabel(
                text = label,
                fontSize = labelSize,
                modifier = Modifier
                    .width(250.dp)
                    .padding(end = 18.dp),
            )
            Row(modifier = Modifier.weight(1f), content = content)
        }
    }

    @Composable
    private fun DeviceDropdown() {
        val fallbackAnchor = LocalView.current
        var menuAnchor by remember { mutableStateOf<View?>(null) }
        Row(verticalAlignment = Alignment.CenterVertically) {
            Box(modifier = Modifier.weight(1f)) {
                AndroidView(
                    factory = { context ->
                        View(context).also { menuAnchor = it }
                    },
                    modifier = Modifier
                        .align(Alignment.BottomStart)
                        .size(1.dp),
                )
                DropdownValue(
                    text = selectedDevice?.name ?: getString(R.string.hint_no_capture_devices),
                    hint = getString(R.string.hint_select_device),
                    modifier = Modifier.fillMaxWidth(),
                    onClick = { showDeviceMenu(menuAnchor ?: fallbackAnchor) },
                )
            }
            if (selectedDeviceCompatibilityIssue() != null) {
                IconButton(onClick = {
                    selectedDeviceCompatibilityIssue()?.let { showCompatibilityIssueDialog(it) }
                }) {
                    Icon(
                        painter = painterResource(R.drawable.ic_warning),
                        contentDescription = getString(R.string.action_capture_card_compatibility_warning),
                        tint = Color(0xFFFFE082),
                    )
                }
            }
        }
    }

    @Composable
    private fun ResolutionDropdown() {
        val fallbackAnchor = LocalView.current
        var menuAnchor by remember { mutableStateOf<View?>(null) }
        Box {
            AndroidView(
                factory = { context ->
                    View(context).also { menuAnchor = it }
                },
                modifier = Modifier
                    .align(Alignment.BottomStart)
                    .size(1.dp),
            )
            DropdownValue(
                text = resolutionDropdownText,
                hint = getString(R.string.hint_select_resolution),
                modifier = Modifier.fillMaxWidth(),
                onClick = {
                    if (probedFormatSizes.isEmpty()) {
                        refreshResolutions()
                    } else {
                        showResolutionFormatMenu(menuAnchor ?: fallbackAnchor)
                    }
                },
            )
        }
    }

    @Composable
    private fun DropdownValue(
        text: String,
        hint: String,
        modifier: Modifier = Modifier,
        onClick: () -> Unit,
        menu: @Composable () -> Unit = {},
    ) {
        Box(modifier = modifier) {
            Row(
                verticalAlignment = Alignment.CenterVertically,
                modifier = Modifier
                    .fillMaxWidth()
                    .clickable { onClick() }
                    .border(0.dp, Color.Transparent)
                    .padding(top = 8.dp, bottom = 7.dp),
            ) {
                Text(
                    text = text.ifBlank { hint },
                    color = if (text.isBlank()) Color(0xB3FFFFFF) else Color.White,
                    fontSize = 16.sp,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                    modifier = Modifier.weight(1f),
                )
                Text("▾", color = Color.White, fontSize = 18.sp)
            }
            Box(
                modifier = Modifier
                    .align(Alignment.BottomStart)
                    .fillMaxWidth()
                    .height(1.dp)
                    .background(Color.White),
            )
            menu()
        }
    }

    @Composable
    private fun PlaybackControlsBar(modifier: Modifier = Modifier) {
        Row(
            verticalAlignment = Alignment.CenterVertically,
            modifier = modifier
                .background(Color(0xDD222222), RoundedCornerShape(42.dp))
                .border(1.dp, Color(0xEE444444), RoundedCornerShape(42.dp))
                .padding(horizontal = 16.dp, vertical = 8.dp),
        ) {
            IconButton(onClick = { stopPlaybackFromControls() }, modifier = Modifier.size(42.dp)) {
                Icon(
                    painterResource(R.drawable.ic_power),
                    getString(R.string.action_stop_video),
                    tint = Color(0xFFFF453A),
                )
            }
            BarDivider()
            IconButton(onClick = { toggleMute() }, modifier = Modifier.size(36.dp)) {
                Icon(
                    painterResource(if (audioMuted || audioVolumePercent == 0) R.drawable.ic_volume_off else R.drawable.ic_volume_up),
                    null,
                    tint = Color.White,
                )
            }
            WhiteSlider(
                value = if (audioMuted) 0f else audioVolumePercent.toFloat(),
                onValueChange = {
                    if (it > 0f) audioMuted = false
                    audioVolumePercent = it.roundToInt().coerceIn(0, 100)
                    applyAudioVolumeFromUi()
                    resetControlsTimer()
                },
                onValueChangeFinished = {
                    persistSettings()
                    resetControlsTimer()
                },
            )
            BarDivider()
            Icon(painterResource(R.drawable.ic_zoom_out), null, tint = Color.White, modifier = Modifier.size(24.dp))
            WhiteSlider(
                value = currentZoom.toFloat(),
                onValueChange = {
                    currentZoom = it.roundToInt().coerceIn(0, 100)
                    resetControlsTimer()
                },
                onValueChangeFinished = {
                    persistSettings()
                    resetControlsTimer()
                },
            )
            Icon(painterResource(R.drawable.ic_zoom_in), null, tint = Color.White, modifier = Modifier.size(24.dp))
            BarDivider()
            IconButton(
                onClick = {
                    showSettingsDialog()
                    resetControlsTimer()
                },
                modifier = Modifier.size(42.dp),
            ) {
                Icon(painterResource(R.drawable.ic_settings), null, tint = Color.White)
            }
        }
    }

    @Composable
    private fun WhiteSlider(
        value: Float,
        onValueChange: (Float) -> Unit,
        onValueChangeFinished: () -> Unit,
    ) {
        Slider(
            value = value,
            onValueChange = onValueChange,
            onValueChangeFinished = onValueChangeFinished,
            valueRange = 0f..100f,
            colors = SliderDefaults.colors(
                thumbColor = Color.White,
                activeTrackColor = Color.White,
                inactiveTrackColor = Color(0x66FFFFFF),
            ),
            modifier = Modifier.width(120.dp),
        )
    }

    @Composable
    private fun BarDivider() {
        Spacer(
            modifier = Modifier
                .padding(horizontal = 12.dp)
                .width(1.dp)
                .height(32.dp)
                .background(Color(0x33FFFFFF)),
        )
    }

    @Composable
    private fun ModalBackdrop(content: @Composable () -> Unit) {
        Box(
            modifier = Modifier
                .fillMaxSize()
                .background(Color(0x99000000))
                .padding(16.dp),
            contentAlignment = Alignment.Center,
        ) {
            content()
        }
    }

    @Composable
    private fun ModalPanel(
        width: Dp,
        content: @Composable ColumnScope.() -> Unit,
    ) {
        BoxWithConstraints(contentAlignment = Alignment.Center) {
            Column(
                modifier = Modifier
                    .widthIn(max = width)
                    .fillMaxWidth()
                    .heightIn(max = maxHeight - 32.dp)
                    .background(Color(0xFF191919), RoundedCornerShape(28.dp))
                    .border(1.dp, Color(0x44FFFFFF), RoundedCornerShape(28.dp))
                    .padding(24.dp)
                    .verticalScroll(rememberScrollState()),
                content = content,
            )
        }
    }

    @Composable
    private fun HelpSheet() {
        ModalPanel(width = 700.dp) {
            ModalHeader(getString(R.string.help_title), iconSize = 48.dp)
            DividerLine()
            InfoRow(R.drawable.ic_about_play_circle, getString(R.string.help_getting_started_title), getString(R.string.help_getting_started_body))
            InfoRow(R.drawable.ic_help_running, getString(R.string.help_frame_rate_title), getString(R.string.help_frame_rate_body))
            InfoRow(R.drawable.ic_help_video_controls, getString(R.string.help_video_controls_title), getString(R.string.help_video_controls_body))
            InfoRow(R.drawable.ic_volume_up, getString(R.string.help_audio_controls_title), getString(R.string.help_audio_controls_body))
            InfoRow(R.drawable.ic_help_device_support, getString(R.string.help_device_support_title), getString(R.string.help_device_support_body))
            DividerLine()
            ModalActions {
                Spacer(Modifier.weight(1f))
                CloseButton()
            }
        }
    }

    @Composable
    private fun AboutSheet() {
        val context = LocalContext.current
        ModalPanel(width = 635.dp) {
            ModalHeader(
                getString(R.string.about_header_title, AppBuildInfo.version),
                subtitle = getString(R.string.about_copyright),
                iconSize = 64.dp,
            )
            BodyText(getString(R.string.about_trademark_body), Modifier.padding(top = 18.dp))
            DividerLine()
            InfoRow(R.drawable.ic_about_play_circle, null, getString(R.string.about_utility_body))
            InfoRow(R.drawable.ic_about_warning, null, getString(R.string.about_uvc_required_body))
            InfoRow(R.drawable.ic_about_shield, null, getString(R.string.about_privacy_body))
            InfoRow(R.drawable.ic_about_heart, null, getString(R.string.about_open_source_body))
            InfoRow(R.drawable.ic_about_document, null, getString(R.string.about_build_info_label))
            Box(
                modifier = Modifier
                    .fillMaxWidth()
                    .background(Color(0x1AFFFFFF), RoundedCornerShape(8.dp))
                    .padding(14.dp),
            ) {
                Text(
                    text = getString(
                        R.string.about_build_info_display,
                        AppBuildInfo.version,
                        AppBuildInfo.buildType,
                        AppBuildInfo.buildDate,
                        AppBuildInfo.commit,
                    ),
                    color = Color.White,
                    fontSize = 14.sp,
                    fontFamily = FontFamily.Monospace,
                    modifier = Modifier.padding(end = 180.dp),
                )
                SmallPillButton(
                    text = getString(R.string.action_copy_to_clipboard),
                    modifier = Modifier.align(Alignment.TopEnd),
                ) {
                    val clipboard = getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
                    clipboard.setPrimaryClip(
                        ClipData.newPlainText(
                            getString(R.string.about_build_info_clip_label),
                            AppBuildInfo.copyableBlob,
                        ),
                    )
                    showMessage(getString(R.string.message_build_info_copied))
                }
            }
            DividerLine()
            ModalActions {
                SmallPillButton(getString(R.string.about_github_button)) {
                    context.startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(GITHUB_URL)))
                }
                Spacer(Modifier.width(8.dp))
                SmallPillButton(getString(R.string.about_privacy_policy_button)) {
                    context.startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(PRIVACY_POLICY_URL)))
                }
                Spacer(Modifier.weight(1f))
                CloseButton()
            }
        }
    }

    @Composable
    private fun SettingsSheet() {
        ModalPanel(width = 620.dp) {
            ModalHeader(getString(R.string.settings_title), iconSize = 48.dp)
            DividerLine()
            SectionTitle(getString(R.string.settings_section_performance_telemetry))
            Row(
                verticalAlignment = Alignment.CenterVertically,
                modifier = Modifier.fillMaxWidth(),
            ) {
                Text(
                    text = getString(R.string.settings_show_stats),
                    color = Color.White,
                    fontSize = 15.sp,
                    fontWeight = FontWeight.Bold,
                    modifier = Modifier.padding(end = 18.dp),
                )
                InlineRadioOption(
                    getString(R.string.settings_stats_off),
                    statsPosition == StatsPosition.OFF,
                    Modifier.padding(end = 28.dp),
                ) {
                    statsPosition = StatsPosition.OFF
                    isStatsVisible = false
                    updateStatsOverlay(previewBackend.getTelemetry())
                    persistSettings()
                }
                InlineRadioOption(
                    getString(R.string.settings_stats_bottom_left),
                    statsPosition == StatsPosition.BOTTOM_LEFT,
                    Modifier.padding(end = 28.dp),
                ) {
                    statsPosition = StatsPosition.BOTTOM_LEFT
                    isStatsVisible = true
                    updateStatsOverlay(previewBackend.getTelemetry())
                    persistSettings()
                }
                InlineRadioOption(
                    getString(R.string.settings_stats_bottom_right),
                    statsPosition == StatsPosition.BOTTOM_RIGHT,
                    Modifier.padding(end = 28.dp),
                ) {
                    statsPosition = StatsPosition.BOTTOM_RIGHT
                    isStatsVisible = true
                    updateStatsOverlay(previewBackend.getTelemetry())
                    persistSettings()
                }
            }
            Row(
                verticalAlignment = Alignment.CenterVertically,
                modifier = Modifier.fillMaxWidth(),
            ) {
                SwitchSetting(
                    getString(R.string.settings_low_fps_warnings),
                    isLowFpsWarningEnabled,
                    Modifier.padding(end = 36.dp),
                ) {
                    isLowFpsWarningEnabled = it
                    if (!it) {
                        lowFpsBelowThresholdSinceMs = 0L
                        isLowFpsWarningVisible = false
                    }
                    persistSettings()
                }
                SwitchSetting(
                    getString(R.string.settings_show_debug_stats),
                    isDebugStatsEnabled,
                    Modifier.padding(end = 36.dp),
                    enabled = statsPosition != StatsPosition.OFF,
                ) {
                    isDebugStatsEnabled = it
                    updateStatsOverlay(previewBackend.getTelemetry())
                    persistSettings()
                }
            }
            SectionTitle(getString(R.string.settings_section_video_transformation))
            Row(
                verticalAlignment = Alignment.CenterVertically,
                modifier = Modifier.fillMaxWidth(),
            ) {
                Text(
                    text = getString(R.string.settings_rotate_label),
                    color = Color.White,
                    fontSize = 15.sp,
                    fontWeight = FontWeight.Bold,
                    modifier = Modifier.padding(end = 18.dp),
                )
                listOf(0, 90, 180, 270).forEach { rotation ->
                    InlineRadioOption(
                        getString(R.string.settings_rotation_degrees, rotation),
                        currentRotation == rotation,
                        Modifier.padding(end = 28.dp),
                    ) {
                        currentRotation = rotation
                        persistSettings()
                    }
                }
            }
            Row(
                verticalAlignment = Alignment.CenterVertically,
                modifier = Modifier.fillMaxWidth(),
            ) {
                Text(
                    text = getString(R.string.settings_flip_label),
                    color = Color.White,
                    fontSize = 15.sp,
                    fontWeight = FontWeight.Bold,
                    modifier = Modifier.padding(end = 18.dp),
                )
                SwitchSetting(
                    getString(R.string.settings_flip_horizontal_short),
                    isFlippedHorizontal,
                    Modifier.padding(end = 36.dp),
                ) {
                    isFlippedHorizontal = it
                    persistSettings()
                }
                SwitchSetting(
                    getString(R.string.settings_flip_vertical_short),
                    isFlippedVertical,
                    Modifier.padding(end = 36.dp),
                ) {
                    isFlippedVertical = it
                    persistSettings()
                }
            }
            SwitchSetting(getString(R.string.settings_picture_in_picture), isPipEnabled) {
                isPipEnabled = it
                updatePipParams()
                persistSettings()
            }
            DividerLine()
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                SmallPillButton(getString(R.string.action_help)) { activeSheet = ActiveSheet.HELP }
                SmallPillButton(getString(R.string.action_about)) { activeSheet = ActiveSheet.ABOUT }
                Spacer(Modifier.weight(1f))
                CloseButton()
            }
        }
    }

    @Composable
    private fun ResolutionPickerSheet() {
        ModalPanel(width = 640.dp) {
            ModalHeader(getString(R.string.label_resolution), iconSize = 48.dp)
            DividerLine()
            resolutionChoices().forEach { choice ->
                RadioSetting(
                    text = getString(
                        R.string.resolution_choice_format,
                        choice.width,
                        choice.height,
                        choice.fps.roundToInt(),
                        formatMenuLabel(choice.pixelPreference, choice.unsafeDebug),
                    ),
                    selected = selectedFormat?.let {
                        it.width == choice.width &&
                            it.height == choice.height &&
                            abs(it.getCurrentFrameRate() - choice.fps) <= DEFAULT_TARGET_FPS_TOLERANCE &&
                            selectedPixelFormatPreference == choice.pixelPreference
                    } == true,
                ) {
                    selectResolutionChoice(choice)
                    showResolutionPicker = false
                }
            }
            DividerLine()
            ModalActions {
                SmallPillButton(getString(R.string.action_refresh)) {
                    showResolutionPicker = false
                    refreshResolutions()
                }
                Spacer(Modifier.weight(1f))
                CloseButton { showResolutionPicker = false }
            }
        }
    }

    @Composable
    private fun CompatibilitySheet() {
        val issue = selectedDeviceCompatibilityIssue()
        ModalPanel(width = 640.dp) {
            WarningHeader(getString(R.string.compatibility_issue_title))
            DividerLine()
            BodyText(getString(R.string.compatibility_issue_intro))
            SectionTitle(getString(R.string.compatibility_issue_about_title))
            BodyText(issue?.summary.orEmpty(), Modifier.padding(top = 10.dp))
            DividerLine()
            ModalActions { CloseButton() }
        }
    }

    @Composable
    private fun MessageSheet(
        title: String,
        message: String,
        closeText: String = getString(R.string.action_close),
        onClose: () -> Unit = { activeSheet = null },
    ) {
        ModalPanel(width = 520.dp) {
            ModalHeader(title, iconSize = 48.dp)
            DividerLine()
            BodyText(message, Modifier.padding(top = 18.dp))
            DividerLine()
            ModalActions {
                Spacer(Modifier.weight(1f))
                SmallPillButton(closeText, onClick = onClose)
            }
        }
    }

    @Composable
    private fun ModalHeader(title: String, subtitle: String? = null, iconSize: Dp) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Image(
                painter = painterResource(R.drawable.app_icon_large),
                contentDescription = null,
                contentScale = ContentScale.Crop,
                modifier = Modifier
                    .size(iconSize)
                    .clip(RoundedCornerShape(if (iconSize >= 64.dp) 14.dp else 10.dp)),
            )
            Spacer(Modifier.width(14.dp))
            Column {
                Text(title, color = Color.White, fontSize = 30.sp, fontWeight = FontWeight.Bold)
                subtitle?.let { BodyText(it, Modifier.padding(top = 4.dp)) }
            }
        }
    }

    @Composable
    private fun WarningHeader(title: String) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Icon(painterResource(R.drawable.ic_warning), null, tint = Color(0xFFFFE082), modifier = Modifier.size(42.dp))
            Spacer(Modifier.width(14.dp))
            Text(title, color = Color.White, fontSize = 28.sp, fontWeight = FontWeight.Bold)
        }
    }

    @Composable
    private fun InfoRow(iconRes: Int, title: String?, body: String) {
        Row(modifier = Modifier.padding(top = 4.dp, bottom = 14.dp)) {
            Icon(
                painter = painterResource(iconRes),
                contentDescription = null,
                tint = Color(0xB8FFFFFF),
                modifier = Modifier
                    .padding(top = 1.dp, end = 14.dp)
                    .size(24.dp),
            )
            Column {
                title?.let {
                    Text(it, color = Color.White, fontSize = 17.sp, fontWeight = FontWeight.Bold)
                }
                BodyText(body, Modifier.padding(top = if (title == null) 0.dp else 4.dp))
            }
        }
    }

    @Composable
    private fun SectionTitle(text: String) {
        Text(
            text = text,
            color = Color.White,
            fontSize = 18.sp,
            fontWeight = FontWeight.Bold,
            modifier = Modifier.padding(top = 8.dp, bottom = 8.dp),
        )
    }

    @Composable
    private fun BodyText(text: String, modifier: Modifier = Modifier) {
        Text(
            text = text,
            color = Color(0xB8FFFFFF),
            fontSize = 15.sp,
            lineHeight = 19.sp,
            modifier = modifier,
        )
    }

    @Composable
    private fun DividerLine(modifier: Modifier = Modifier) {
        Spacer(
            modifier = modifier
                .fillMaxWidth()
                .padding(top = 8.dp, bottom = 8.dp)
                .height(1.dp)
                .background(Color(0x33FFFFFF)),
        )
    }

    @Composable
    private fun ModalActions(content: @Composable RowScope.() -> Unit) {
        Row(
            verticalAlignment = Alignment.CenterVertically,
            modifier = Modifier
                .fillMaxWidth()
                .padding(top = 4.dp),
            content = content,
        )
    }

    @Composable
    private fun CloseButton(onClick: () -> Unit = { activeSheet = null }) {
        SmallPillButton(getString(R.string.action_close), onClick = onClick)
    }

    @Composable
    private fun SmallPillButton(
        text: String,
        modifier: Modifier = Modifier,
        onClick: () -> Unit,
    ) {
        Button(
            onClick = onClick,
            colors = ButtonDefaults.buttonColors(containerColor = Color(0x33FFFFFF)),
            shape = RoundedCornerShape(18.dp),
            contentPadding = androidx.compose.foundation.layout.PaddingValues(horizontal = 18.dp, vertical = 7.dp),
            modifier = modifier.heightIn(min = 0.dp),
        ) {
            Text(text, color = Color.White, fontSize = 15.sp, fontWeight = FontWeight.Bold)
        }
    }

    @Composable
    private fun OutlinePillButton(text: String, modifier: Modifier = Modifier, onClick: () -> Unit) {
        Button(
            onClick = onClick,
            colors = ButtonDefaults.buttonColors(containerColor = Color(0x22FFFFFF)),
            shape = RoundedCornerShape(24.dp),
            modifier = modifier.height(48.dp),
            border = androidx.compose.foundation.BorderStroke(1.dp, Color(0x55FFFFFF)),
        ) {
            Text(text, color = Color.White)
        }
    }

    @Composable
    private fun TextIconButton(text: String, iconRes: Int, onClick: () -> Unit) {
        Button(
            onClick = onClick,
            colors = ButtonDefaults.textButtonColors(contentColor = Color.White),
            shape = RoundedCornerShape(50),
            border = androidx.compose.foundation.BorderStroke(1.dp, Color.White.copy(alpha = 0.6f)),
            contentPadding = androidx.compose.foundation.layout.PaddingValues(horizontal = 16.dp, vertical = 0.dp),
        ) {
            Icon(painterResource(iconRes), null, tint = Color.White, modifier = Modifier.size(18.dp))
            Spacer(Modifier.width(8.dp))
            Text(text, color = Color.White)
        }
    }

    @Composable
    private fun RadioSetting(text: String, selected: Boolean, onClick: () -> Unit) {
        Row(
            verticalAlignment = Alignment.CenterVertically,
            modifier = Modifier
                .fillMaxWidth()
                .clickable { onClick() }
                .padding(vertical = 5.dp),
        ) {
            RadioButton(
                selected = selected,
                onClick = onClick,
                colors = RadioButtonDefaults.colors(
                    selectedColor = MaterialTheme.colorScheme.primary,
                    unselectedColor = Color(0x99FFFFFF),
                ),
            )
            Text(text, color = Color.White, fontSize = 15.sp)
        }
    }

    @Composable
    private fun InlineRadioOption(
        text: String,
        selected: Boolean,
        modifier: Modifier = Modifier,
        onClick: () -> Unit,
    ) {
        Row(
            verticalAlignment = Alignment.CenterVertically,
            modifier = modifier
                .clickable { onClick() }
                .padding(vertical = 3.dp),
        ) {
            RadioButton(
                selected = selected,
                onClick = onClick,
                colors = RadioButtonDefaults.colors(
                    selectedColor = MaterialTheme.colorScheme.primary,
                    unselectedColor = Color(0x99FFFFFF),
                ),
            )
            Text(text, color = Color.White, fontSize = 14.sp, maxLines = 1)
        }
    }

    @Composable
    private fun SwitchSetting(
        text: String,
        checked: Boolean,
        modifier: Modifier = Modifier,
        enabled: Boolean = true,
        onCheckedChange: (Boolean) -> Unit,
    ) {
        val contentColor = if (enabled) Color.White else Color(0x66FFFFFF)
        Row(
            verticalAlignment = Alignment.CenterVertically,
            modifier = modifier
                .clickable(enabled = enabled) { onCheckedChange(!checked) }
                .padding(vertical = 7.dp),
        ) {
            Text(text, color = contentColor, fontSize = 15.sp)
            Spacer(Modifier.width(10.dp))
            Switch(
                checked = checked,
                onCheckedChange = onCheckedChange,
                enabled = enabled,
                colors = SwitchDefaults.colors(
                    checkedThumbColor = Color.White,
                    checkedTrackColor = MaterialTheme.colorScheme.primary,
                    uncheckedThumbColor = Color(0xCCFFFFFF),
                    uncheckedTrackColor = Color(0x33222222),
                    disabledCheckedThumbColor = Color(0x66FFFFFF),
                    disabledCheckedTrackColor = Color(0x33CC1199),
                    disabledUncheckedThumbColor = Color(0x66FFFFFF),
                    disabledUncheckedTrackColor = Color(0x22222222),
                ),
            )
        }
    }

    private fun updatePipParams() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            val params = PictureInPictureParams.Builder()
                .setAutoEnterEnabled(isPipEnabled && captureEngine.state.value is CaptureState.Running)
                .build()
            setPictureInPictureParams(params)
        }
    }

    private fun updateSystemUiForPlayback(isPlaybackActive: Boolean) {
        WindowCompat.setDecorFitsSystemWindows(window, !isPlaybackActive)
        val controller = WindowInsetsControllerCompat(window, window.decorView)
        if (isPlaybackActive) {
            controller.systemBarsBehavior =
                WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
            controller.hide(WindowInsetsCompat.Type.systemBars())
        } else {
            controller.show(WindowInsetsCompat.Type.systemBars())
        }
    }

    override fun onUserLeaveHint() {
        super.onUserLeaveHint()
        if (isPipEnabled && captureEngine.state.value is CaptureState.Running) {
            enterPictureInPictureMode(PictureInPictureParams.Builder().build())
        }
    }

    override fun onPictureInPictureModeChanged(isInPictureInPictureMode: Boolean, newConfig: Configuration) {
        super.onPictureInPictureModeChanged(isInPictureInPictureMode, newConfig)
        if (isInPictureInPictureMode) {
            areControlsVisible = false
        } else {
            if (captureEngine.state.value is CaptureState.Running) {
                showControls()
            }
        }
    }

    override fun onDestroy() {
        telemetryJob?.cancel()
        controlsAutoHideJob?.cancel()
        cancelConnectingWatchdog()
        cancelAutoStartPlayback()
        previewBackend.setCaptureAudioFailureListener(null)
        deviceRepository.stop()
        previewBackend.dispose()
        super.onDestroy()
    }

    companion object {
        /** Same tag as [org.centennialoss.consolation.preview.backend.UvccameraLibPreviewBackend] probe logs. */
        private const val RESOLUTION_PROBE_TAG = "ConsolationUvcProbe"

        /** Stop/play sequencing; filter `adb logcat -s ConsolationPlayback:I`. */
        private const val PLAYBACK_DIAG_TAG = "ConsolationPlayback"
        /** Per-second compact telemetry line; stripped from release via AppLog policy. */
        private const val TELEMETRY_LOG_TAG = "ConsolationTelemetry"
        private const val GITHUB_URL = "https://github.com/centennial-oss/consolation-android"
        private const val PRIVACY_POLICY_URL = "https://centennialoss.org/privacy/"

        private const val PREFS_NAME = "consolation_ui"
        private const val KEY_STATS_POSITION = "stats_position"
        private const val KEY_STATS_VISIBLE = "stats_visible"
        private const val KEY_LOW_FPS_WARN = "low_fps_warn"
        private const val KEY_SHOW_DEBUG_STATS = "show_debug_stats"
        private const val KEY_PIP_ENABLED = "pip_enabled"
        private const val KEY_ROTATION = "rotation"
        private const val KEY_FLIP_H = "flip_h"
        private const val KEY_FLIP_V = "flip_v"
        private const val KEY_ZOOM = "zoom"
        private const val KEY_VOLUME = "volume"
        private const val KEY_MUTED = "muted"
        private const val KEY_DEVICE_FORMAT_PREFIX = "device_format:"
        private const val MENU_ID_RESOLUTION_BASE = 10_000

        /**
         * Common output modes shown first in the resolution menu and used for automatic
         * format selection. Order is highest to lowest total pixel count within this set.
         */
        private val STANDARD_RESOLUTION_ORDERED: List<Pair<Int, Int>> = listOf(
            3840 to 2160,
            2560 to 1440,
            1920 to 1080,
            1280 to 720,
        )

        private val STANDARD_RESOLUTION_KEYS: Set<Pair<Int, Int>> =
            STANDARD_RESOLUTION_ORDERED.toSet()

        private fun isStandardResolution(width: Int, height: Int): Boolean =
            (width to height) in STANDARD_RESOLUTION_KEYS

        private const val LOW_FPS_MIN_DELTA = 10
        private const val LOW_FPS_SUSTAIN_MS = 3_000L
        private const val CONNECTING_RETRY_TIMEOUT_MS = 7_000L
        private const val POST_USB_PERMISSION_SETTLE_MS = 1_500L
        private const val AUTO_START_PLAYBACK_DELAY_MS = 2_000L
        private const val DEFAULT_TARGET_FPS = 60f
        private const val DEFAULT_TARGET_FPS_TOLERANCE = 0.75f
        private const val MENU_FPS_DEDUP_TOLERANCE = 0.75f

        private data class ResolutionGroup(
            val width: Int,
            val height: Int,
            val fpsOptions: List<Float>,
        )

        private fun sortResolutionGroups(groups: List<ResolutionGroup>): List<ResolutionGroup> {
            val byKey = groups.associateBy { it.width to it.height }
            val standardFirst = STANDARD_RESOLUTION_ORDERED.mapNotNull { byKey[it] }
            val niche = groups
                .filter { (it.width to it.height) !in STANDARD_RESOLUTION_KEYS }
                .sortedWith(
                    compareByDescending<ResolutionGroup> { it.width }
                        .thenByDescending { it.height },
                )
            return standardFirst + niche
        }

        private fun groupSizesByResolution(sizes: List<Size>): List<ResolutionGroup> {
            val map = linkedMapOf<Pair<Int, Int>, MutableList<Size>>()
            for (s in sizes) {
                val key = s.width to s.height
                map.getOrPut(key) { mutableListOf() }.add(s)
            }
            val unsorted = map.map { (k, v) ->
                val fpsOptions = collapseFpsOptions(
                    v.flatMap { it.fps?.toList().orEmpty() },
                    MENU_FPS_DEDUP_TOLERANCE,
                )
                ResolutionGroup(k.first, k.second, fpsOptions)
            }
            return sortResolutionGroups(unsorted)
        }

        /**
         * Device descriptors sometimes publish duplicate/near-duplicate intervals (e.g. 60 and 59.94)
         * that collapse to the same UI label. Keep one representative per tolerance bucket.
         */
        private fun collapseFpsOptions(values: List<Float>, tolerance: Float): List<Float> {
            if (values.isEmpty()) return emptyList()
            val sorted = values.sortedDescending()
            val collapsed = mutableListOf<Float>()
            for (fps in sorted) {
                if (collapsed.none { abs(it - fps) <= tolerance }) {
                    collapsed += fps
                }
            }
            return collapsed
        }

        private fun supportedFormatPreferences(sizes: List<Size>): List<PixelFormatPreference> {
            val supported = sizes.mapNotNull { preferenceForFrameType(it.frame_type) }.toSet()
            val ordered = listOf(
                PixelFormatPreference.H264,
                PixelFormatPreference.NV12,
                PixelFormatPreference.YU12,
                PixelFormatPreference.YUYV,
                PixelFormatPreference.P010,
                PixelFormatPreference.BGR3,
                PixelFormatPreference.MJPEG,
            ).filter { it in supported }
            return listOf(PixelFormatPreference.AUTO) + ordered
        }

        private fun supportedFormatPreferencesForResolutionAndFps(
            sizes: List<Size>,
            width: Int,
            height: Int,
            fps: Float,
        ): List<PixelFormatPreference> {
            val candidates = sizes.filter { size ->
                if (size.width != width || size.height != height) return@filter false
                val fpsArray = size.fps ?: return@filter false
                fpsArray.any { abs(it - fps) <= DEFAULT_TARGET_FPS_TOLERANCE }
            }
            return supportedFormatPreferences(candidates)
        }

        private fun preferenceForFrameType(frameType: Int): PixelFormatPreference? = when (frameType) {
            UVCCamera.FRAME_FORMAT_H264 -> PixelFormatPreference.H264
            UVCCamera.FRAME_FORMAT_NV12 -> PixelFormatPreference.NV12
            UVCCamera.FRAME_FORMAT_YUYV -> PixelFormatPreference.YUYV
            UVCCamera.FRAME_FORMAT_P010 -> PixelFormatPreference.P010
            UVCCamera.FRAME_FORMAT_YU12 -> PixelFormatPreference.YU12
            UVCCamera.FRAME_FORMAT_BGR3 -> PixelFormatPreference.BGR3
            UVCCamera.FRAME_FORMAT_MJPEG -> PixelFormatPreference.MJPEG
            else -> null
        }

        private fun formatMenuLabel(pref: PixelFormatPreference, unsafeDebug: Boolean = false): String {
            val base = when (pref) {
                PixelFormatPreference.AUTO -> "Auto"
                PixelFormatPreference.H264 -> "H264"
                PixelFormatPreference.NV12 -> "NV12"
                PixelFormatPreference.YUYV -> "YUYV"
                PixelFormatPreference.P010 -> "P010"
                PixelFormatPreference.YU12 -> "YU12"
                PixelFormatPreference.BGR3 -> "BGR3"
                PixelFormatPreference.MJPEG -> "MJPEG"
            }
            return if (unsafeDebug) "$base (DEBUG / UNSAFE)" else base
        }

        /** True if this exact mode exists on the probe list (never matches DEBUG-synthesized entries). */
        private fun reportedProbeOverlapsCandidate(reportedSizes: List<Size>, candidate: Size): Boolean {
            return reportedSizes.any { r ->
                r.width == candidate.width &&
                    r.height == candidate.height &&
                    r.frame_type == candidate.frame_type &&
                    (r.fps ?: floatArrayOf()).any { rf ->
                        (candidate.fps ?: floatArrayOf()).any { sf ->
                            abs(rf - sf) <= DEFAULT_TARGET_FPS_TOLERANCE
                        }
                    }
            }
        }

        private fun isDebugUnsafeFormatChoice(
            reportedSizes: List<Size>,
            width: Int,
            height: Int,
            fps: Float,
            pref: PixelFormatPreference,
        ): Boolean {
            if (!BuildConfig.DEBUG) return false
            val frameType = pref.frameFormat ?: return false
            if (frameType != UVCCamera.FRAME_FORMAT_NV12 &&
                frameType != UVCCamera.FRAME_FORMAT_YUYV &&
                frameType != UVCCamera.FRAME_FORMAT_YU12
            ) {
                return false
            }
            return reportedSizes.none { size ->
                size.width == width &&
                    size.height == height &&
                    size.frame_type == frameType &&
                    (size.fps ?: floatArrayOf()).any { abs(it - fps) <= DEFAULT_TARGET_FPS_TOLERANCE }
            }
        }

        private fun applyDebugUnsafeFormatSynthesis(reportedSizes: List<Size>): List<Size> {
            if (!BuildConfig.DEBUG || reportedSizes.isEmpty()) return reportedSizes
            val expanded = reportedSizes.map { Size(it) }.toMutableList()
            val synthFormats = listOf(
                UVCCamera.FRAME_FORMAT_NV12,
                UVCCamera.FRAME_FORMAT_YUYV,
                UVCCamera.FRAME_FORMAT_YU12,
            )
            val donors = synthFormats.associateWith { frameType ->
                reportedSizes.firstOrNull { it.frame_type == frameType }
            }
            if (donors.values.all { it == null }) return expanded

            for (group in groupSizesByResolution(reportedSizes)) {
                for (fps in group.fpsOptions) {
                    for (frameType in synthFormats) {
                        val donor = donors[frameType] ?: continue
                        val exists = expanded.any { size ->
                            size.width == group.width &&
                                size.height == group.height &&
                                size.frame_type == frameType &&
                                (size.fps ?: floatArrayOf()).any { abs(it - fps) <= DEFAULT_TARGET_FPS_TOLERANCE }
                        }
                        if (exists) continue

                        val interval100ns = (10_000_000.0f / fps).toInt().coerceAtLeast(1)
                        expanded += Size(
                            donor.type,
                            frameType,
                            donor.index,
                            group.width,
                            group.height,
                            intArrayOf(interval100ns),
                        )
                    }
                }
            }
            return expanded
        }

        private fun addMenuHeaderWithDivider(menu: Menu, title: String) {
            val styled = SpannableString(title).apply {
                setSpan(ForegroundColorSpan(AndroidColor.parseColor("#80FFFFFF")), 0, length, 0)
            }
            menu.add(Menu.NONE, Menu.NONE, Menu.NONE, styled).isEnabled = false
        }

        private fun pickDefaultFormat(
            sizes: List<Size>,
            compatibilityIssue: DeviceCompatibilityIssue? = null,
            reportedProbeSizes: List<Size>,
        ): Size? {
            if (sizes.isEmpty()) return null
            val groups = groupSizesByResolution(sizes)
            compatibilityIssue?.defaultFormat?.let { defaultFormat ->
                val matchingGroup = groups.firstOrNull {
                    it.width == defaultFormat.width && it.height == defaultFormat.height
                }
                val fps = matchingGroup?.fpsOptions?.minByOrNull { abs(it - defaultFormat.frameRate) }
                    ?.takeIf { abs(it - defaultFormat.frameRate) <= DEFAULT_TARGET_FPS_TOLERANCE }
                if (fps != null) {
                    return resolveFormatChoiceForPreference(
                        sizes,
                        defaultFormat.width,
                        defaultFormat.height,
                        PixelFormatPreference.AUTO,
                        fps,
                        reportedProbeSizes = reportedProbeSizes,
                    )
                }
            }

            val groupsByResolution = groups.associateBy { it.width to it.height }
            val standardGroups = STANDARD_RESOLUTION_ORDERED.mapNotNull { groupsByResolution[it] }
            if (standardGroups.isNotEmpty()) {
                val best60pStandard = standardGroups.firstNotNullOfOrNull { group ->
                    val fps = group.fpsOptions.minByOrNull { abs(it - DEFAULT_TARGET_FPS) }
                        ?.takeIf { abs(it - DEFAULT_TARGET_FPS) <= DEFAULT_TARGET_FPS_TOLERANCE }
                    fps?.let { Triple(group.width, group.height, it) }
                }
                if (best60pStandard != null) {
                    val (width, height, fps) = best60pStandard
                    return resolveFormatChoiceForPreference(
                        sizes,
                        width,
                        height,
                        PixelFormatPreference.AUTO,
                        fps,
                        reportedProbeSizes = reportedProbeSizes,
                    )
                }
                val topStandard = standardGroups.first()
                val maxFps = topStandard.fpsOptions.maxOrNull() ?: return null
                return resolveFormatChoiceForPreference(
                    sizes,
                    topStandard.width,
                    topStandard.height,
                    PixelFormatPreference.AUTO,
                    maxFps,
                    reportedProbeSizes = reportedProbeSizes,
                )
            }

            val best60p = groups.firstNotNullOfOrNull { group ->
                val fps = group.fpsOptions.minByOrNull { abs(it - DEFAULT_TARGET_FPS) }
                    ?.takeIf { abs(it - DEFAULT_TARGET_FPS) <= DEFAULT_TARGET_FPS_TOLERANCE }
                fps?.let { Triple(group.width, group.height, it) }
            }
            if (best60p != null) {
                val (width, height, fps) = best60p
                return resolveFormatChoiceForPreference(
                    sizes,
                    width,
                    height,
                    PixelFormatPreference.AUTO,
                    fps,
                    reportedProbeSizes = reportedProbeSizes,
                )
            }

            val best = groups.firstOrNull() ?: return Size(sizes.first())
            val maxFps = best.fpsOptions.maxOrNull() ?: return null
            return resolveFormatChoiceForPreference(
                sizes,
                best.width,
                best.height,
                PixelFormatPreference.AUTO,
                maxFps,
                reportedProbeSizes = reportedProbeSizes,
            )
        }

        private fun resolveFormatChoiceForPreference(
            allSizes: List<Size>,
            width: Int,
            height: Int,
            preference: PixelFormatPreference,
            requestedFps: Float?,
            reportedProbeSizes: List<Size>? = null,
        ): Size? {
            if (allSizes.isEmpty()) return null
            var resolutionCandidates = allSizes.filter { it.width == width && it.height == height }
            val reported = reportedProbeSizes?.takeIf { it.isNotEmpty() }
            if (preference == PixelFormatPreference.AUTO && reported != null) {
                resolutionCandidates =
                    resolutionCandidates.filter { reportedProbeOverlapsCandidate(reported, it) }
            }
            if (resolutionCandidates.isEmpty()) return null
            val prioritized = when (preference) {
                PixelFormatPreference.AUTO ->
                    listOf(
                        UVCCamera.FRAME_FORMAT_NV12,
                        UVCCamera.FRAME_FORMAT_YU12,
                        UVCCamera.FRAME_FORMAT_YUYV,
                        UVCCamera.FRAME_FORMAT_P010,
                        UVCCamera.FRAME_FORMAT_BGR3,
                        UVCCamera.FRAME_FORMAT_MJPEG,
                    )
                else -> listOfNotNull(preference.frameFormat)
            }
            val targetFps = requestedFps ?: DEFAULT_TARGET_FPS
            val fpsMatchedCandidates = resolutionCandidates.filter { size ->
                val fpsArray = size.fps ?: return@filter false
                fpsArray.any { abs(it - targetFps) <= DEFAULT_TARGET_FPS_TOLERANCE }
            }
            val candidates = if (prioritized.isEmpty()) {
                if (fpsMatchedCandidates.isNotEmpty()) fpsMatchedCandidates else resolutionCandidates
            } else {
                val pool = if (fpsMatchedCandidates.isNotEmpty()) fpsMatchedCandidates else resolutionCandidates
                prioritized
                    .asSequence()
                    .mapNotNull { fmt ->
                        pool
                            .filter { it.frame_type == fmt }
                            .maxByOrNull { bestAvailableFps(it) }
                    }
                    .toList()
                    .ifEmpty { pool }
            }
            val template = candidates.minByOrNull { size ->
                val fpsArray = size.fps ?: return@minByOrNull Float.POSITIVE_INFINITY
                fpsArray.minOfOrNull { abs(it - targetFps) } ?: Float.POSITIVE_INFINITY
            } ?: candidates.maxByOrNull { bestAvailableFps(it) } ?: return null
            val copy = Size(template)
            val fpsArray = copy.fps ?: return copy
            var bestIdx = 0
            var bestDiff = Float.POSITIVE_INFINITY
            for (i in fpsArray.indices) {
                val d = abs(fpsArray[i] - targetFps)
                if (d < bestDiff) {
                    bestDiff = d
                    bestIdx = i
                }
            }
            copy.frameIntervalIndex = bestIdx.coerceIn(0, fpsArray.lastIndex)
            return copy
        }

        private fun bestAvailableFps(size: Size): Float = size.fps?.maxOrNull() ?: 0f

        private fun formatResolutionLabel(size: Size, pref: PixelFormatPreference): String {
            val fps = try {
                size.getCurrentFrameRate().roundToInt()
            } catch (_: Exception) {
                0
            }
            val base = "${size.width}x${size.height} @ ${fps}p"
            return if (pref == PixelFormatPreference.AUTO) {
                base
            } else {
                "$base (${formatMenuLabel(pref)})"
            }
        }
    }
}
