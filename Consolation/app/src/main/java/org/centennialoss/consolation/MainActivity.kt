package org.centennialoss.consolation

import android.Manifest
import android.app.Dialog
import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.content.Intent
import android.content.SharedPreferences
import android.content.pm.PackageManager
import android.graphics.Color
import android.graphics.Outline
import android.graphics.drawable.ColorDrawable
import android.net.Uri
import android.os.Bundle
import android.util.TypedValue
import android.view.ContextThemeWrapper
import android.view.Menu
import android.view.MotionEvent
import android.view.TextureView
import android.view.View
import android.view.ViewOutlineProvider
import android.view.WindowManager
import android.widget.ArrayAdapter
import android.widget.Button
import android.widget.FrameLayout
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import androidx.constraintlayout.widget.ConstraintLayout
import androidx.activity.ComponentActivity
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.widget.PopupMenu as AppCompatPopupMenu
import androidx.core.content.ContextCompat
import androidx.core.view.doOnLayout
import androidx.core.view.isVisible
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
import org.centennialoss.consolation.databinding.ActivityMainBinding
import org.centennialoss.consolation.databinding.PlaybackControlsBinding
import org.centennialoss.consolation.preview.backend.UsbVideoPreviewBackend
import org.centennialoss.consolation.preview.backend.UsbVideoPreviewBackendFactory
import org.centennialoss.consolation.usb.UsbCaptureDeviceRepository
import org.centennialoss.consolation.logging.AppLog as Log
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlin.math.abs
import kotlin.math.roundToInt
import java.util.Locale

class MainActivity : ComponentActivity() {
    private lateinit var binding: ActivityMainBinding
    private lateinit var previewTexture: TextureView
    private lateinit var deviceRepository: UsbCaptureDeviceRepository
    private lateinit var previewBackend: UsbVideoPreviewBackend
    private val captureEngine = NoopCaptureEngine()

    private lateinit var prefs: SharedPreferences

    private var selectedDevice: CaptureDevice? = null

    /** Raw formats from the last successful probe for the current device (independent of open camera). */
    private var probedFormatSizes: List<Size> = emptyList()

    /** User choice: width, height, and frame interval index into [Size.fps]. */
    private var selectedFormat: Size? = null

    private var permissionTimeoutJob: Job? = null
    private var telemetryJob: Job? = null
    private var controlsAutoHideJob: Job? = null
    private var connectingWatchdogJob: Job? = null
    private var hasRetriedConnectingSession = false
    private var lastUsbPermissionGrantedAtMs = 0L

    private var lastResolutionRefreshDeviceId: String? = null
    private var lastResolutionRefreshHadUsbPermission: Boolean = false

    // Settings
    private var isStatsVisible = false
    private var statsPosition = StatsPosition.OFF
    private var isLowFpsWarningEnabled = true
    private var currentRotation = 0
    private var isFlippedHorizontal = false
    private var isFlippedVertical = false
    private var currentZoom = 0

    private var audioVolumePercent = 100
    private var audioMuted = false
    private var volumeBeforeMute = 100

    private var lowFpsBelowThresholdSinceMs: Long = 0L

    enum class StatsPosition { OFF, BOTTOM_LEFT, BOTTOM_RIGHT }

    private enum class RuntimePermissionAction { REQUEST_USB_PERMISSION, START_WATCH }

    private var pendingRuntimePermissionAction: RuntimePermissionAction? = null
    private var startWatchAfterUsbPermission = false

    private val requestCameraRecordForWatch = registerForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions(),
    ) { results ->
        val pending = selectedDevice ?: return@registerForActivityResult
        if (!hasRuntimeCameraPermission() && results[Manifest.permission.CAMERA] != true) {
            pendingRuntimePermissionAction = null
            startWatchAfterUsbPermission = false
            showMessage(getString(R.string.message_camera_permission_required_for_usb))
            return@registerForActivityResult
        }
        when (pendingRuntimePermissionAction) {
            RuntimePermissionAction.REQUEST_USB_PERMISSION -> requestUsbPermissionForSelection(autoStartAfterGrant = false)
            RuntimePermissionAction.START_WATCH -> lifecycleScope.launch {
                startWatchSession(pending)
            }
            null -> Unit
        }
        pendingRuntimePermissionAction = null
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityMainBinding.inflate(layoutInflater)
        previewTexture = binding.previewTexture
        prefs = getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
        loadSettingsFromPrefs()
        setContentView(binding.root)

        deviceRepository = UsbCaptureDeviceRepository(this)
        previewBackend = UsbVideoPreviewBackendFactory.create(this, deviceRepository)
        previewBackend.setCaptureAudioFailureListener {
            showCaptureAudioFailureDialog()
        }

        setupStartupScreen()
        setupPlaybackControls()
        setupMainTouchListener()
        observeState()

        deviceRepository.refreshAfterUsbIntent(intent)
        updatePreviewScale()
    }

    override fun onStart() {
        super.onStart()
        deviceRepository.start()
        if (captureEngine.state.value is CaptureState.Running) {
            Log.i(
                PLAYBACK_DIAG_TAG,
                "activity onStart while running -> rebind preview surface",
            )
            previewBackend.bindPreviewSurface(previewTexture)
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

    private fun setupStartupScreen() {
        val startup = binding.startupScreen
        val cornerPx =
            TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, 16f, resources.displayMetrics)
        applyRoundedIconClip(startup.startupAppIcon, cornerPx)
        startup.appVersionText.text = "v${AppBuildInfo.version}"

        // ExposedDropdownMenu wires the end icon to AutoCompleteTextView.showDropDown(). Resolution
        // uses a PopupMenu instead, so the default chevron toggled without opening anything; device
        // benefits from an explicit showDropDown() when the field is not focusable.
        startup.deviceInputLayout.setEndIconOnClickListener {
            startup.deviceDropdown.showDropDown()
        }
        startup.resolutionInputLayout.setEndIconOnClickListener {
            showResolutionFormatMenu(startup.resolutionDropdown)
        }

        startup.deviceDropdown.setOnItemClickListener { _, _, position, _ ->
            val devices = deviceRepository.devices.value
            val device = devices.getOrNull(position)
            if (device?.id != selectedDevice?.id) {
                selectedFormat = null
                probedFormatSizes = emptyList()
                startup.resolutionDropdown.setText("", false)
            }
            selectedDevice = device
            lastResolutionRefreshDeviceId = null
            updateCompatibilityWarning()
            refreshResolutions()
        }

        startup.compatibilityWarningButton.setOnClickListener {
            selectedDeviceCompatibilityIssue()?.let { issue ->
                showCompatibilityIssueDialog(issue)
            }
        }

        startup.requestUsbPermissionButton.setOnClickListener {
            requestUsbPermissionForSelection(autoStartAfterGrant = false)
        }

        startup.resolutionDropdown.setOnClickListener {
            showResolutionFormatMenu(startup.resolutionDropdown)
        }

        startup.playButton.setOnClickListener {
            handlePlayAction()
        }

        startup.helpButton.setOnClickListener { showHelpDialog() }
        startup.aboutButton.setOnClickListener { showAboutDialog() }
    }

    private fun setupPlaybackControls() {
        val controls = binding.playbackControls

        controls.stopButton.setOnClickListener {
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
                    previewBackend.unbindPreviewSurface()
                }
                Log.i(
                    PLAYBACK_DIAG_TAG,
                    "stop unbind+complete totalMs=${android.os.SystemClock.elapsedRealtime() - t0}",
                )
            }
        }

        controls.muteButton.setOnClickListener {
            audioMuted = !audioMuted
            if (audioMuted) {
                volumeBeforeMute = controls.volumeSeekBar.progress.coerceAtLeast(1)
                controls.volumeSeekBar.progress = 0
                applyAudioVolumeFromUi()
            } else {
                controls.volumeSeekBar.progress = volumeBeforeMute
                applyAudioVolumeFromUi()
            }
            updateMuteButtonIcon(controls)
            resetControlsTimer()
            persistSettings()
        }

        controls.volumeSeekBar.progress = if (audioMuted) 0 else audioVolumePercent
        controls.volumeSeekBar.setOnSeekBarChangeListener(object : android.widget.SeekBar.OnSeekBarChangeListener {
            override fun onProgressChanged(seekBar: android.widget.SeekBar?, progress: Int, fromUser: Boolean) {
                if (fromUser && progress > 0) {
                    audioMuted = false
                }
                if (!audioMuted) {
                    audioVolumePercent = progress
                }
                applyAudioVolumeFromUi()
                updateMuteButtonIcon(controls)
            }

            override fun onStartTrackingTouch(seekBar: android.widget.SeekBar?) {
                resetControlsTimer()
            }

            override fun onStopTrackingTouch(seekBar: android.widget.SeekBar?) {
                persistSettings()
                resetControlsTimer()
            }
        })

        controls.zoomSeekBar.progress = currentZoom
        controls.zoomSeekBar.setOnSeekBarChangeListener(object : android.widget.SeekBar.OnSeekBarChangeListener {
            override fun onProgressChanged(seekBar: android.widget.SeekBar?, progress: Int, fromUser: Boolean) {
                currentZoom = progress
                updatePreviewScale()
            }

            override fun onStartTrackingTouch(seekBar: android.widget.SeekBar?) {
                resetControlsTimer()
            }

            override fun onStopTrackingTouch(seekBar: android.widget.SeekBar?) {
                persistSettings()
                resetControlsTimer()
            }
        })

        updateMuteButtonIcon(controls)
        controls.settingsButton.setOnClickListener { showSettingsMenu(it) }
    }

    private fun applyAudioVolumeFromUi() {
        val controls = binding.playbackControls
        val linear = if (audioMuted) {
            0f
        } else {
            (controls.volumeSeekBar.progress / 100f).coerceIn(0f, 1f)
        }
        previewBackend.setCaptureAudioVolume(linear)
    }

    private fun updateMuteButtonIcon(controls: PlaybackControlsBinding) {
        controls.muteButton.setImageResource(
            if (audioMuted || controls.volumeSeekBar.progress == 0) {
                R.drawable.ic_volume_off
            } else {
                R.drawable.ic_volume_up
            },
        )
    }

    private fun setupMainTouchListener() {
        binding.mainContainer.setOnTouchListener { _, event ->
            if (event.action == MotionEvent.ACTION_DOWN) {
                if (captureEngine.state.value is CaptureState.Running) {
                    showControls()
                }
            }
            false
        }
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
            requestCameraRecordForWatch.launch(
                arrayOf(Manifest.permission.CAMERA, Manifest.permission.RECORD_AUDIO),
            )
            return
        }

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
    private data class WatchSessionPrep(val format: Size?, val probedUpdate: List<Size>?)

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
                            showMessage(getString(R.string.message_permission_denied))
                            updateStartupActions()
                        }
                    }
                }
            }
        }
    }

    private fun updateUiForState(devices: List<CaptureDevice>, state: CaptureState) {
        val isRunning = state is CaptureState.Running
        binding.startupScreen.root.isVisible = !isRunning
        binding.playbackControls.root.isVisible = isRunning
        previewTexture.isVisible = isRunning

        if (!isRunning) {
            telemetryJob?.cancel()
            cancelConnectingWatchdog()
            hasRetriedConnectingSession = false
            lowFpsBelowThresholdSinceMs = 0L
            binding.lowFpsWarning.isVisible = false
            binding.videoStatsOverlay.isVisible = false

            val names = devices.map { it.name }
            val adapter = ArrayAdapter(this, android.R.layout.simple_list_item_1, names)
            binding.startupScreen.deviceDropdown.setAdapter(adapter)

            if (devices.isEmpty()) {
                selectedDevice = null
                selectedFormat = null
                probedFormatSizes = emptyList()
                binding.startupScreen.deviceDropdown.setText(getString(R.string.hint_no_capture_devices), false)
                binding.startupScreen.resolutionDropdown.setText("", false)
                updateCompatibilityWarning()
                updateStartupActions()
            } else {
                if (selectedDevice == null || devices.none { it.id == selectedDevice!!.id }) {
                    selectedDevice = devices.first()
                    selectedFormat = null
                    probedFormatSizes = emptyList()
                    binding.startupScreen.deviceDropdown.setText(selectedDevice?.name, false)
                    binding.startupScreen.resolutionDropdown.setText("", false)
                    lastResolutionRefreshDeviceId = null
                }
                updateCompatibilityWarning()
                maybeRefreshResolutionsAfterDeviceOrPermissionChange()
                updateStartupActions()
            }

        } else {
            startTelemetryLoop()
            showControls()
            binding.playbackControls.volumeSeekBar.progress = if (audioMuted) 0 else audioVolumePercent
            applyAudioVolumeFromUi()
            updateMuteButtonIcon(binding.playbackControls)
            window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        }

        if (!isRunning) {
            window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        }

        updateConnectingOverlay(isRunning)
    }

    private fun replacePreviewTextureAfterUsbRemoval() {
        val oldView = previewTexture
        oldView.surfaceTextureListener = null
        oldView.isVisible = false
        val parent = oldView.parent as? ConstraintLayout ?: return
        val index = parent.indexOfChild(oldView)
        val params = oldView.layoutParams as ConstraintLayout.LayoutParams
        parent.removeView(oldView)
        previewTexture = TextureView(this).apply {
            id = R.id.previewTexture
            layoutParams = ConstraintLayout.LayoutParams(params)
            isVisible = false
        }
        parent.addView(previewTexture, index)
        updatePreviewScale()
    }

    private fun updateConnectingOverlay(isRunning: Boolean) {
        binding.connectingCaptureCardLabel.isVisible =
            isRunning && !previewBackend.hasReceivedFirstVideoFrame()
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
        binding.startupScreen.compatibilityWarningButton.isVisible =
            selectedDeviceCompatibilityIssue() != null
    }

    private fun updateStartupActions() {
        val device = selectedDevice
        val hasDevice = device != null
        val hasUsbPermission = device?.let { deviceRepository.hasPermission(it) } == true
        val hasResolution = selectedFormat != null
        val needsAccess = hasDevice && (
            !hasUsbPermission ||
                !hasRuntimeCameraPermission() ||
                !hasRuntimeRecordAudioPermission()
            )
        val isRequestingPermission = captureEngine.state.value is CaptureState.RequestingPermission

        binding.startupScreen.permissionNoticeText.isVisible = needsAccess
        binding.startupScreen.permissionNoticeDivider.isVisible = needsAccess
        binding.startupScreen.resolutionLabel.isVisible = hasUsbPermission
        binding.startupScreen.resolutionInputLayout.isVisible = hasUsbPermission
        binding.startupScreen.requestUsbPermissionButton.isVisible = hasDevice && !hasUsbPermission
        binding.startupScreen.requestUsbPermissionButton.isEnabled = !isRequestingPermission
        binding.startupScreen.playButton.isEnabled = hasDevice && hasUsbPermission && hasResolution
        binding.startupScreen.playButton.alpha =
            if (binding.startupScreen.playButton.isEnabled) 1.0f else 0.4f
    }

    private fun refreshResolutions() {
        val device = selectedDevice ?: run {
            Log.d(RESOLUTION_PROBE_TAG, "refreshResolutions: skip (no selected device)")
            updateStartupActions()
            return
        }
        if (!deviceRepository.hasPermission(device)) {
            Log.d(
                RESOLUTION_PROBE_TAG,
                "refreshResolutions: skip (no USB permission) id=${device.id} name=${device.name}",
            )
            binding.startupScreen.resolutionDropdown.setAdapter(null)
            binding.startupScreen.resolutionDropdown.setText("", false)
            probedFormatSizes = emptyList()
            selectedFormat = null
            updateStartupActions()
            return
        }

        updateStartupActions()
        binding.startupScreen.resolutionDropdown.setText(
            getString(R.string.state_checking_formats),
            false,
        )

        Log.i(
            RESOLUTION_PROBE_TAG,
            "refreshResolutions: scheduling probe id=${device.id} name=${device.name}",
        )
        lifecycleScope.launch {
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
            probedFormatSizes = sizes
            if (sizes.isNotEmpty()) {
                previewBackend.consumeLastProbeOpenFailed()
                val defaultFormat =
                    loadRememberedFormat(device, sizes)
                        ?: pickDefaultFormat(sizes, selectedDeviceCompatibilityIssue())
                        ?: sizes.first().let { Size(it) }
                selectedFormat = defaultFormat
                val label = formatResolutionLabel(defaultFormat)
                binding.startupScreen.resolutionDropdown.setText(label, false)
                updateStartupActions()
                Log.i(
                    RESOLUTION_PROBE_TAG,
                    "refreshResolutions: UI updated size=${sizes.size} defaultLabel=$label",
                )
            } else {
                val probeOpenFailed = previewBackend.consumeLastProbeOpenFailed()
                selectedFormat = null
                binding.startupScreen.resolutionDropdown.setText(
                    getString(
                        if (probeOpenFailed) {
                            R.string.state_probe_unavailable
                        } else {
                            R.string.state_no_formats
                        },
                    ),
                    false,
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
        }
    }

    private fun showResolutionFormatMenu(anchor: View) {
        if (probedFormatSizes.isEmpty()) {
            refreshResolutions()
            return
        }
        val popup = AppCompatPopupMenu(popupMenuContext(), anchor)
        val menu = popup.menu
        val choiceIds = mutableMapOf<Int, Triple<Int, Int, Float>>()
        var nextId = MENU_ID_RESOLUTION_BASE
        for (group in groupSizesByResolution(probedFormatSizes)) {
            val sub: Menu = menu.addSubMenu(Menu.NONE, Menu.NONE, Menu.NONE, "${group.width}x${group.height}")
            for (fps in group.fpsOptions) {
                val id = nextId++
                choiceIds[id] = Triple(group.width, group.height, fps)
                sub.add(Menu.NONE, id, Menu.NONE, "${fps.roundToInt()} fps")
            }
        }
        popup.setOnMenuItemClickListener { item ->
            val triple = choiceIds[item.itemId] ?: return@setOnMenuItemClickListener false
            val (w, h, fps) = triple
            selectedFormat = resolveFormatChoice(probedFormatSizes, w, h, fps)
            selectedDevice?.let { device -> persistFormatForDevice(device, selectedFormat!!) }
            binding.startupScreen.resolutionDropdown.setText(
                formatResolutionLabel(selectedFormat!!),
                false,
            )
            updateStartupActions()
            true
        }
        popup.show()
    }

    private fun updateAspectRatio(width: Int, height: Int) {
        val params = previewTexture.layoutParams as ConstraintLayout.LayoutParams
        params.dimensionRatio = "$width:$height"
        previewTexture.layoutParams = params
    }

    private fun startTelemetryLoop() {
        telemetryJob?.cancel()
        telemetryJob = lifecycleScope.launch {
            while (true) {
                val stats = previewBackend.getTelemetry()
                updateConnectingOverlay(captureEngine.state.value is CaptureState.Running)
                if (previewBackend.hasReceivedFirstVideoFrame()) {
                    cancelConnectingWatchdog()
                }
                updateStatsOverlay(stats)
                updateLowFpsWarning(stats)
                delay(500)
            }
        }
    }

    private fun updateStatsOverlay(stats: org.centennialoss.consolation.core.telemetry.TelemetrySnapshot) {
        binding.videoStatsOverlay.isVisible = isStatsVisible && statsPosition != StatsPosition.OFF
        if (binding.videoStatsOverlay.isVisible) {
            binding.videoStatsOverlay.text = getString(
                R.string.telemetry_format,
                stats.width, stats.height, stats.configuredFps,
                stats.pixelFormat, stats.fps, stats.droppedFrames,
                stats.nativePreviewConversionAvgMs,
                formatTelemetryPayloadLabel(stats.nativePayloadAvgKb),
            )
            val params = binding.videoStatsOverlay.layoutParams as androidx.constraintlayout.widget.ConstraintLayout.LayoutParams
            if (statsPosition == StatsPosition.BOTTOM_LEFT) {
                params.startToStart = androidx.constraintlayout.widget.ConstraintLayout.LayoutParams.PARENT_ID
                params.endToEnd = androidx.constraintlayout.widget.ConstraintLayout.LayoutParams.UNSET
            } else {
                params.startToStart = androidx.constraintlayout.widget.ConstraintLayout.LayoutParams.UNSET
                params.endToEnd = androidx.constraintlayout.widget.ConstraintLayout.LayoutParams.PARENT_ID
            }
            binding.videoStatsOverlay.layoutParams = params
        }
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

    private fun updateLowFpsWarning(stats: org.centennialoss.consolation.core.telemetry.TelemetrySnapshot) {
        if (!previewBackend.hasReceivedFirstVideoFrame()) {
            lowFpsBelowThresholdSinceMs = 0L
            binding.lowFpsWarning.isVisible = false
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
            binding.lowFpsWarning.isVisible = false
            return
        }
        if (lowFpsBelowThresholdSinceMs == 0L) {
            lowFpsBelowThresholdSinceMs = now
        }
        val sustained = now - lowFpsBelowThresholdSinceMs >= LOW_FPS_SUSTAIN_MS
        val showWarning = sustained
        binding.lowFpsWarning.isVisible = showWarning
        if (showWarning) {
            val params = binding.lowFpsWarning.layoutParams as androidx.constraintlayout.widget.ConstraintLayout.LayoutParams
            val statsBottomLeft = isStatsVisible && statsPosition == StatsPosition.BOTTOM_LEFT
            if (statsBottomLeft) {
                params.startToStart = androidx.constraintlayout.widget.ConstraintLayout.LayoutParams.UNSET
                params.endToEnd = androidx.constraintlayout.widget.ConstraintLayout.LayoutParams.PARENT_ID
            } else {
                params.startToStart = androidx.constraintlayout.widget.ConstraintLayout.LayoutParams.PARENT_ID
                params.endToEnd = androidx.constraintlayout.widget.ConstraintLayout.LayoutParams.UNSET
            }
            binding.lowFpsWarning.layoutParams = params
            binding.lowFpsWarning.setOnClickListener { showLowFpsInfo() }
        }
    }

    private fun showControls() {
        binding.playbackControls.root.isVisible = true
        resetControlsTimer()
    }

    private fun resetControlsTimer() {
        controlsAutoHideJob?.cancel()
        controlsAutoHideJob = lifecycleScope.launch {
            delay(3000)
            binding.playbackControls.root.isVisible = false
        }
    }

    private fun updatePreviewScale() {
        val maxScale = 1.175f * 1.5f
        val baseScale = 1.0f + (currentZoom / 100.0f) * (maxScale - 1.0f)
        previewTexture.scaleX = baseScale * (if (isFlippedHorizontal) -1f else 1f)
        previewTexture.scaleY = baseScale * (if (isFlippedVertical) -1f else 1f)
        previewTexture.rotation = currentRotation.toFloat()
    }

    private fun showSettingsMenu(view: View) {
        val popup = AppCompatPopupMenu(popupMenuContext(), view)
        popup.menuInflater.inflate(R.menu.playback_settings, popup.menu)

        popup.menu.findItem(R.id.menu_show_stats_off)?.isChecked = statsPosition == StatsPosition.OFF
        popup.menu.findItem(R.id.menu_show_stats_left)?.isChecked = statsPosition == StatsPosition.BOTTOM_LEFT
        popup.menu.findItem(R.id.menu_show_stats_right)?.isChecked = statsPosition == StatsPosition.BOTTOM_RIGHT
        popup.menu.findItem(R.id.menu_low_fps_warning)?.isChecked = isLowFpsWarningEnabled

        popup.menu.findItem(R.id.menu_rotate_0)?.isChecked = currentRotation == 0
        popup.menu.findItem(R.id.menu_rotate_90)?.isChecked = currentRotation == 90
        popup.menu.findItem(R.id.menu_rotate_180)?.isChecked = currentRotation == 180
        popup.menu.findItem(R.id.menu_rotate_270)?.isChecked = currentRotation == 270

        popup.menu.findItem(R.id.menu_flip_h)?.isChecked = isFlippedHorizontal
        popup.menu.findItem(R.id.menu_flip_v)?.isChecked = isFlippedVertical

        popup.setOnMenuItemClickListener { item ->
            when (item.itemId) {
                R.id.menu_show_stats_off -> {
                    statsPosition = StatsPosition.OFF
                    isStatsVisible = false
                }
                R.id.menu_show_stats_left -> {
                    statsPosition = StatsPosition.BOTTOM_LEFT
                    isStatsVisible = true
                }
                R.id.menu_show_stats_right -> {
                    statsPosition = StatsPosition.BOTTOM_RIGHT
                    isStatsVisible = true
                }
                R.id.menu_low_fps_warning -> {
                    isLowFpsWarningEnabled = !isLowFpsWarningEnabled
                    item.isChecked = isLowFpsWarningEnabled
                    if (!isLowFpsWarningEnabled) {
                        lowFpsBelowThresholdSinceMs = 0L
                        binding.lowFpsWarning.isVisible = false
                    }
                }
                R.id.menu_rotate_0 -> {
                    currentRotation = 0
                    updatePreviewScale()
                }
                R.id.menu_rotate_90 -> {
                    currentRotation = 90
                    updatePreviewScale()
                }
                R.id.menu_rotate_180 -> {
                    currentRotation = 180
                    updatePreviewScale()
                }
                R.id.menu_rotate_270 -> {
                    currentRotation = 270
                    updatePreviewScale()
                }
                R.id.menu_flip_h -> {
                    isFlippedHorizontal = item.isChecked
                    updatePreviewScale()
                }
                R.id.menu_flip_v -> {
                    isFlippedVertical = item.isChecked
                    updatePreviewScale()
                }
                R.id.menu_help -> showHelpDialog()
                R.id.menu_about -> showAboutDialog()
                else -> return@setOnMenuItemClickListener false
            }
            persistSettings()
            true
        }
        popup.show()
    }

    private fun showHelpDialog() {
        val content = modalPanel()
        content.addView(modalHeader(getString(R.string.help_title), iconSizeDp = 48))
        content.addView(modalDivider())

        content.addView(helpSection("play.circle", R.string.help_getting_started_title, R.string.help_getting_started_body))
        content.addView(helpSection("figure.run", R.string.help_frame_rate_title, R.string.help_frame_rate_body))
        content.addView(helpSection("view", R.string.help_video_controls_title, R.string.help_video_controls_body))
        content.addView(helpSection("volume", R.string.help_audio_controls_title, R.string.help_audio_controls_body))
        content.addView(helpSection("usb", R.string.help_device_support_title, R.string.help_device_support_body))

        content.addView(modalDivider())
        showModal(content, widthDp = 640)
    }

    private fun showAboutDialog() {
        val content = modalPanel()
        content.addView(modalHeader(getString(R.string.about_title), subtitle = getString(R.string.about_copyright), iconSizeDp = 64))
        content.addView(modalBody(getString(R.string.about_trademark_body), topMarginDp = 18))
        content.addView(modalDivider())
        content.addView(aboutRow("play", getString(R.string.about_utility_body)))
        content.addView(aboutRow("warning", getString(R.string.about_uvc_required_body)))
        content.addView(aboutRow("shield", getString(R.string.about_privacy_body)))
        content.addView(aboutRow("heart", getString(R.string.about_open_source_body)))
        content.addView(buildInfoSection())
        content.addView(modalDivider())
        showModal(
            content,
            widthDp = 635,
            leadingActions = listOf(
                modalExternalLinkButton(getString(R.string.about_github_button)) {
                    startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(GITHUB_URL)))
                },
                modalExternalLinkButton(getString(R.string.about_privacy_policy_button)) {
                    startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(PRIVACY_POLICY_URL)))
                },
            ),
        )
    }

    private fun showLowFpsInfo() {
        showMessageDialog(
            title = getString(R.string.low_fps_info_title),
            message = getString(R.string.low_fps_info_message),
        )
    }

    private fun showCompatibilityIssueDialog(issue: DeviceCompatibilityIssue) {
        val content = modalPanel()
        content.addView(modalWarningHeader(getString(R.string.compatibility_issue_title)))
        content.addView(modalDivider())
        content.addView(modalBody(getString(R.string.compatibility_issue_intro), topMarginDp = 2))
        content.addView(
            modalSectionTitle(
                getString(R.string.compatibility_issue_about_title),
                topMarginDp = 28,
            ),
        )
        content.addView(modalBody(issue.summary, topMarginDp = 10))
        showModal(content, widthDp = 640)
    }

    private fun showCaptureAudioFailureDialog() {
        if (isFinishing || isDestroyed) {
            return
        }
        showMessageDialog(
            title = getString(R.string.audio_playback_failed_title),
            message = getString(R.string.audio_playback_failed_message),
        )
    }

    private fun showMessageDialog(title: String, message: String) {
        val content = modalPanel()
        content.addView(modalHeader(title, iconSizeDp = 48))
        content.addView(modalDivider())
        content.addView(modalBody(message, topMarginDp = 18))
        showModal(content, widthDp = 520)
    }

    private fun showModal(content: LinearLayout, widthDp: Int, leadingActions: List<View> = emptyList()) {
        val dialog = Dialog(this)
        val scroll = ScrollView(this).apply {
            isFillViewport = false
            setPadding(dp(16), dp(16), dp(16), dp(16))
            addView(content)
        }
        val close = modalCloseButton {
            dialog.dismiss()
        }
        content.addView(
            LinearLayout(this).apply {
                gravity = android.view.Gravity.CENTER_VERTICAL
                setPadding(0, dp(4), 0, 0)
                leadingActions.forEach { action ->
                    addView(
                        action.apply {
                            layoutParams = LinearLayout.LayoutParams(
                                LinearLayout.LayoutParams.WRAP_CONTENT,
                                LinearLayout.LayoutParams.WRAP_CONTENT,
                            ).apply {
                                rightMargin = dp(8)
                            }
                        },
                    )
                }
                addView(
                    View(this@MainActivity).apply {
                        layoutParams = LinearLayout.LayoutParams(0, 1, 1f)
                    },
                )
                addView(close)
            },
        )
        dialog.setContentView(scroll)
        dialog.show()
        dialog.window?.setBackgroundDrawable(ColorDrawable(Color.TRANSPARENT))
        dialog.window?.setLayout(
            minOf(resources.displayMetrics.widthPixels - dp(32), dp(widthDp)),
            WindowManager.LayoutParams.WRAP_CONTENT,
        )
    }

    private fun modalPanel(): LinearLayout =
        LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(24), dp(24), dp(24), dp(24))
            background = ContextCompat.getDrawable(this@MainActivity, R.drawable.bg_modal_panel)
        }

    private fun modalHeader(title: String, subtitle: String? = null, iconSizeDp: Int): LinearLayout =
        LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = android.view.Gravity.CENTER_VERTICAL
            val icon = ImageView(this@MainActivity).apply {
                setImageResource(R.drawable.app_icon_large)
                scaleType = ImageView.ScaleType.CENTER_CROP
                layoutParams = LinearLayout.LayoutParams(dp(iconSizeDp), dp(iconSizeDp))
            }
            applyRoundedIconClip(icon, dp(if (iconSizeDp >= 64) 14 else 10).toFloat())
            addView(icon)
            addView(
                LinearLayout(this@MainActivity).apply {
                    orientation = LinearLayout.VERTICAL
                    setPadding(dp(14), 0, 0, 0)
                    addView(
                        TextView(this@MainActivity).apply {
                            text = if (title == getString(R.string.about_title)) {
                                getString(R.string.about_header_title, AppBuildInfo.version)
                            } else {
                                title
                            }
                            setTextColor(Color.WHITE)
                            setTextSize(TypedValue.COMPLEX_UNIT_SP, 30f)
                            setTypeface(typeface, android.graphics.Typeface.BOLD)
                        },
                    )
                    subtitle?.let {
                        addView(modalBody(it, topMarginDp = 4))
                    }
                },
            )
        }

    private fun modalWarningHeader(title: String): LinearLayout =
        LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = android.view.Gravity.CENTER_VERTICAL
            addView(
                ImageView(this@MainActivity).apply {
                    setImageResource(R.drawable.ic_warning)
                    layoutParams = LinearLayout.LayoutParams(dp(42), dp(42))
                },
            )
            addView(
                TextView(this@MainActivity).apply {
                    text = title
                    setTextColor(Color.WHITE)
                    setTextSize(TypedValue.COMPLEX_UNIT_SP, 28f)
                    setTypeface(typeface, android.graphics.Typeface.BOLD)
                    layoutParams = LinearLayout.LayoutParams(
                        LinearLayout.LayoutParams.WRAP_CONTENT,
                        LinearLayout.LayoutParams.WRAP_CONTENT,
                    ).apply {
                        leftMargin = dp(14)
                    }
                },
            )
        }

    private fun modalSectionTitle(textValue: String, topMarginDp: Int): TextView =
        TextView(this).apply {
            text = textValue
            setTextColor(Color.WHITE)
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 18f)
            setTypeface(typeface, android.graphics.Typeface.BOLD)
            layoutParams = LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT,
            ).apply {
                topMargin = dp(topMarginDp)
            }
        }

    private fun modalDivider(): View =
        View(this).apply {
            setBackgroundColor(Color.parseColor("#33FFFFFF"))
            layoutParams = LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                dp(1),
            ).apply {
                topMargin = dp(20)
                bottomMargin = dp(18)
            }
        }

    private fun modalBody(textValue: String, topMarginDp: Int = 0): TextView =
        TextView(this).apply {
            text = textValue
            setTextColor(Color.parseColor("#B8FFFFFF"))
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 15f)
            setLineSpacing(dp(2).toFloat(), 1.0f)
            layoutParams = LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT,
            ).apply {
                topMargin = dp(topMarginDp)
            }
        }

    private fun helpSection(iconText: String, titleRes: Int, bodyRes: Int): LinearLayout =
        aboutRow(iconText, getString(bodyRes), title = getString(titleRes))

    private fun aboutRow(
        iconText: String,
        body: String,
        title: String? = null,
        iconColor: Int = Color.parseColor("#B8FFFFFF"),
        bodyColor: Int = Color.parseColor("#B8FFFFFF"),
    ): LinearLayout =
        LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            setPadding(0, dp(4), 0, dp(14))
            addView(rowIcon(iconText, iconColor))
            addView(
                LinearLayout(this@MainActivity).apply {
                    orientation = LinearLayout.VERTICAL
                    title?.let {
                        addView(
                            TextView(this@MainActivity).apply {
                                text = it
                                setTextColor(Color.WHITE)
                                setTextSize(TypedValue.COMPLEX_UNIT_SP, 17f)
                                setTypeface(typeface, android.graphics.Typeface.BOLD)
                            },
                        )
                    }
                    addView(
                        modalBody(body, topMarginDp = if (title == null) 0 else 4).apply {
                            setTextColor(bodyColor)
                        },
                    )
                },
            )
        }

    private fun linkRow(iconText: String, label: String, onClick: () -> Unit): LinearLayout =
        aboutRow(
            iconText,
            label,
            iconColor = Color.parseColor("#FF3399FF"),
            bodyColor = Color.parseColor("#FF3399FF"),
        ).apply {
            setOnClickListener { onClick() }
            isClickable = true
        }

    private fun rowIcon(iconText: String, iconColor: Int): ImageView =
        ImageView(this).apply {
            setImageResource(
                when (iconText) {
                    "play", "play.circle" -> R.drawable.ic_about_play_circle
                    "warning" -> R.drawable.ic_about_warning
                    "shield" -> R.drawable.ic_about_shield
                    "heart" -> R.drawable.ic_about_heart
                    "open" -> R.drawable.ic_about_external_link
                    "document" -> R.drawable.ic_about_document
                    "volume" -> R.drawable.ic_volume_up
                    "figure.run" -> R.drawable.ic_help_running
                    "view" -> R.drawable.ic_help_video_controls
                    "usb" -> R.drawable.ic_help_device_support
                    else -> R.drawable.ic_info
                },
            )
            setColorFilter(iconColor)
            scaleType = ImageView.ScaleType.CENTER
            layoutParams = LinearLayout.LayoutParams(dp(24), dp(24)).apply {
                rightMargin = dp(14)
                topMargin = dp(1)
            }
        }

    private fun buildInfoSection(): LinearLayout =
        LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(0, dp(6), 0, dp(10))
            addView(aboutRow("document", getString(R.string.about_build_info_label)))
            addView(
	                FrameLayout(this@MainActivity).apply {
	                    setPadding(dp(14), dp(12), dp(14), dp(12))
	                    background = ContextCompat.getDrawable(this@MainActivity, R.drawable.bg_modal_build_info)
	                    addView(
	                        LinearLayout(this@MainActivity).apply {
	                            orientation = LinearLayout.VERTICAL
	                            layoutParams = FrameLayout.LayoutParams(
	                                FrameLayout.LayoutParams.MATCH_PARENT,
	                                FrameLayout.LayoutParams.WRAP_CONTENT,
	                                android.view.Gravity.START or android.view.Gravity.TOP,
	                            )
	                            addView(
	                                TextView(this@MainActivity).apply {
	                                    text = """
                                                Version: ${AppBuildInfo.version} (Android)
                                                Build Type: ${AppBuildInfo.buildType}
                                                Date: ${AppBuildInfo.buildDate}
                                            """.trimIndent()
	                                    setTextColor(Color.WHITE)
	                                    setTextSize(TypedValue.COMPLEX_UNIT_SP, 14f)
	                                    typeface = android.graphics.Typeface.MONOSPACE
	                                    setPadding(0, 0, dp(190), 0)
	                                },
	                            )
	                            addView(
	                                TextView(this@MainActivity).apply {
	                                    text = "Commit: ${AppBuildInfo.commit}"
	                                    setTextColor(Color.WHITE)
	                                    setTextSize(TypedValue.COMPLEX_UNIT_SP, 14f)
	                                    typeface = android.graphics.Typeface.MONOSPACE
	                                },
	                            )
	                        },
	                    )
	                    addView(
	                        modalPillButton(getString(R.string.action_copy_to_clipboard)) {
	                            val clipboard = getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
	                            clipboard.setPrimaryClip(ClipData.newPlainText("Consolation build info", AppBuildInfo.copyableBlob))
	                            showMessage(getString(R.string.message_build_info_copied))
	                        }.apply {
	                            layoutParams = FrameLayout.LayoutParams(
	                                FrameLayout.LayoutParams.WRAP_CONTENT,
	                                FrameLayout.LayoutParams.WRAP_CONTENT,
	                                android.view.Gravity.END or android.view.Gravity.TOP,
	                            ).apply {
	                                leftMargin = dp(16)
	                            }
	                        },
	                    )
	                },
	            )
	        }

    private fun modalCloseButton(onClick: () -> Unit): Button =
        modalPillButton(getString(R.string.action_close), onClick)

    private fun modalExternalLinkButton(textValue: String, onClick: () -> Unit): Button =
        modalPillButton(textValue, onClick).apply {
            val icon = ContextCompat.getDrawable(this@MainActivity, R.drawable.ic_about_external_link)?.mutate()
            icon?.setTint(Color.WHITE)
            setCompoundDrawablesWithIntrinsicBounds(icon, null, null, null)
            compoundDrawablePadding = dp(8)
        }

    private fun modalPillButton(textValue: String, onClick: () -> Unit): Button =
        Button(this).apply {
            text = textValue
            isAllCaps = false
            setTextColor(Color.WHITE)
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 15f)
            setTypeface(typeface, android.graphics.Typeface.BOLD)
            background = ContextCompat.getDrawable(this@MainActivity, R.drawable.bg_modal_pill_button)
            minHeight = 0
            minimumHeight = 0
            minWidth = 0
            minimumWidth = 0
            setPadding(dp(18), dp(7), dp(18), dp(7))
            setOnClickListener { onClick() }
        }

    private fun dp(value: Int): Int =
        TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, value.toFloat(), resources.displayMetrics).roundToInt()

    private fun popupMenuContext(): Context =
        ContextThemeWrapper(this, R.style.ThemeOverlay_Consolation_PopupMenu)

    private fun applyRoundedIconClip(view: View, cornerPx: Float) {
        view.doOnLayout { host ->
            host.outlineProvider =
                object : ViewOutlineProvider() {
                    override fun getOutline(view: View, outline: Outline) {
                        outline.setRoundRect(0, 0, view.width, view.height, cornerPx)
                    }
                }
            host.clipToOutline = true
        }
    }

    private fun persistFormatForDevice(device: CaptureDevice, format: Size) {
        val fps = try {
            format.getCurrentFrameRate().roundToInt().coerceAtLeast(1)
        } catch (_: Exception) {
            return
        }
        prefs.edit()
            .putString(formatPreferenceKey(device), "${format.width},${format.height},$fps")
            .apply()
    }

    private fun loadRememberedFormat(device: CaptureDevice, sizes: List<Size>): Size? {
        val parts = prefs.getString(formatPreferenceKey(device), null)
            ?.split(',')
            ?: return null
        if (parts.size != 3) return null
        val width = parts[0].toIntOrNull() ?: return null
        val height = parts[1].toIntOrNull() ?: return null
        val fps = parts[2].toFloatOrNull() ?: return null
        if (sizes.none { it.width == width && it.height == height }) return null
        return resolveFormatChoice(sizes, width, height, fps)
    }

    private fun formatPreferenceKey(device: CaptureDevice): String {
        return "$KEY_DEVICE_FORMAT_PREFIX${device.vendorId}:${device.productId}:${device.name}"
    }

    private suspend fun startWatchSession(device: CaptureDevice) {
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
            if (format == null || snapshotProbedEmpty) {
                newProbed = previewBackend.probeSupportedSizes(device)
                format =
                    loadRememberedFormat(device, newProbed)
                        ?: pickDefaultFormat(newProbed, DeviceCompatibilityIssues.issueFor(device))
                        ?: newProbed.firstOrNull()?.let { Size(it) }
            }
            if (format == null) {
                return@withContext WatchSessionPrep(null, newProbed ?: emptyList())
            }

            val fps = try {
                format.getCurrentFrameRate().roundToInt().coerceAtLeast(1)
            } catch (_: Exception) {
                30
            }
            previewBackend.setPreviewSize(format.width, format.height, fps)

            WatchSessionPrep(format, newProbed)
        }

        if (prep.format == null) {
            prep.probedUpdate?.let { probedFormatSizes = it }
            selectedFormat = null
            showMessage(getString(R.string.state_no_formats))
            return
        }

        prep.probedUpdate?.let { probedFormatSizes = it }
        selectedFormat = prep.format
        persistFormatForDevice(device, prep.format)

        updateAspectRatio(prep.format.width, prep.format.height)
        hasRetriedConnectingSession = false
        captureEngine.startWatching(device)
        binding.startupScreen.root.isVisible = false
        binding.playbackControls.root.isVisible = true
        previewTexture.isVisible = true
        previewTexture.doOnLayout {
            previewBackend.bindPreviewSurface(previewTexture)
        }
        startConnectingWatchdog(device, prep.format)
    }

    private fun startConnectingWatchdog(device: CaptureDevice, format: Size) {
        cancelConnectingWatchdog()
        connectingWatchdogJob = lifecycleScope.launch {
            delay(CONNECTING_RETRY_TIMEOUT_MS)
            if (captureEngine.state.value !is CaptureState.Running) return@launch
            if (previewBackend.hasReceivedFirstVideoFrame()) return@launch
            if (hasRetriedConnectingSession) return@launch

            hasRetriedConnectingSession = true
            Log.w(
                PLAYBACK_DIAG_TAG,
                "connecting watchdog: no first frame after ${CONNECTING_RETRY_TIMEOUT_MS}ms; retrying preview open",
            )
            val fps = try {
                format.getCurrentFrameRate().roundToInt().coerceAtLeast(1)
            } catch (_: Exception) {
                30
            }
            withContext(Dispatchers.IO) {
                previewBackend.unbindPreviewSurface()
                previewBackend.configureSession(device)
                previewBackend.setPreviewSize(format.width, format.height, fps)
            }
            if (captureEngine.state.value !is CaptureState.Running) return@launch
            if (selectedDevice?.id != device.id) return@launch
            updateAspectRatio(format.width, format.height)
            previewTexture.isVisible = true
            previewTexture.doOnLayout {
                previewBackend.bindPreviewSurface(previewTexture)
            }
        }
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
        Snackbar.make(binding.root, message, Snackbar.LENGTH_SHORT).show()
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
            .putInt(KEY_ROTATION, currentRotation)
            .putBoolean(KEY_FLIP_H, isFlippedHorizontal)
            .putBoolean(KEY_FLIP_V, isFlippedVertical)
            .putInt(KEY_ZOOM, currentZoom)
            .putInt(KEY_VOLUME, audioVolumePercent)
            .putBoolean(KEY_MUTED, audioMuted)
            .apply()
    }

    override fun onDestroy() {
        telemetryJob?.cancel()
        controlsAutoHideJob?.cancel()
        cancelConnectingWatchdog()
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
        private const val GITHUB_URL = "https://github.com/centennial-oss/consolation-android"
        private const val PRIVACY_POLICY_URL = "https://centennialoss.org/privacy/"

        private const val PREFS_NAME = "consolation_ui"
        private const val KEY_STATS_POSITION = "stats_position"
        private const val KEY_STATS_VISIBLE = "stats_visible"
        private const val KEY_LOW_FPS_WARN = "low_fps_warn"
        private const val KEY_ROTATION = "rotation"
        private const val KEY_FLIP_H = "flip_h"
        private const val KEY_FLIP_V = "flip_v"
        private const val KEY_ZOOM = "zoom"
        private const val KEY_VOLUME = "volume"
        private const val KEY_MUTED = "muted"
        private const val KEY_DEVICE_FORMAT_PREFIX = "device_format:"

        private const val MENU_ID_RESOLUTION_BASE = 10_000

        private const val LOW_FPS_MIN_DELTA = 10
        private const val LOW_FPS_SUSTAIN_MS = 3_000L
        private const val CONNECTING_RETRY_TIMEOUT_MS = 7_000L
        private const val POST_USB_PERMISSION_SETTLE_MS = 1_500L
        private const val DEFAULT_TARGET_FPS = 60f
        private const val DEFAULT_TARGET_FPS_TOLERANCE = 0.75f

        private data class ResolutionGroup(val width: Int, val height: Int, val fpsOptions: List<Float>)

        private fun groupSizesByResolution(sizes: List<Size>): List<ResolutionGroup> {
            val map = linkedMapOf<Pair<Int, Int>, MutableList<Float>>()
            for (s in sizes) {
                val fpsArr = s.fps ?: continue
                val key = s.width to s.height
                map.getOrPut(key) { mutableListOf() }.addAll(fpsArr.toList())
            }
            return map.map { (k, v) ->
                ResolutionGroup(k.first, k.second, v.distinct().sortedDescending())
            }.sortedByDescending { it.width * it.height }
        }

        private fun pickDefaultFormat(
            sizes: List<Size>,
            compatibilityIssue: DeviceCompatibilityIssue? = null,
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
                    return resolveFormatChoice(sizes, defaultFormat.width, defaultFormat.height, fps)
                }
            }

            val best60p = groups.firstNotNullOfOrNull { group ->
                val fps = group.fpsOptions.minByOrNull { abs(it - DEFAULT_TARGET_FPS) }
                    ?.takeIf { abs(it - DEFAULT_TARGET_FPS) <= DEFAULT_TARGET_FPS_TOLERANCE }
                fps?.let { Triple(group.width, group.height, it) }
            }
            if (best60p != null) {
                val (width, height, fps) = best60p
                return resolveFormatChoice(sizes, width, height, fps)
            }

            val best = groups.firstOrNull() ?: return Size(sizes.first())
            val maxFps = best.fpsOptions.maxOrNull() ?: return null
            return resolveFormatChoice(sizes, best.width, best.height, maxFps)
        }

        private fun resolveFormatChoice(allSizes: List<Size>, width: Int, height: Int, fps: Float): Size {
            val candidates = allSizes.filter { it.width == width && it.height == height }
            val template = candidates.minByOrNull { size ->
                val fpsArray = size.fps ?: return@minByOrNull Float.POSITIVE_INFINITY
                fpsArray.minOfOrNull { abs(it - fps) } ?: Float.POSITIVE_INFINITY
            } ?: allSizes.first()
            val copy = Size(template)
            val fpsArray = copy.fps ?: return copy
            var bestIdx = 0
            var bestDiff = Float.POSITIVE_INFINITY
            for (i in fpsArray.indices) {
                val d = abs(fpsArray[i] - fps)
                if (d < bestDiff) {
                    bestDiff = d
                    bestIdx = i
                }
            }
            copy.frameIntervalIndex = bestIdx.coerceIn(0, fpsArray.lastIndex)
            return copy
        }

        private fun formatResolutionLabel(size: Size): String {
            val fps = try {
                size.getCurrentFrameRate().roundToInt()
            } catch (_: Exception) {
                0
            }
            return "${size.width}x${size.height} @ ${fps}p"
        }
    }
}
