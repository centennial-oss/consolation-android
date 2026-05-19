package org.centennialoss.consolation.preview.backend

import android.content.Context
import org.centennialoss.consolation.core.capture.CaptureDevice
import org.centennialoss.consolation.core.preview.PreviewRenderer
import org.centennialoss.consolation.usb.UsbCaptureDeviceRepository

interface UsbVideoPreviewBackend : PreviewRenderer {
    val telemetryBackendLabel: String

    fun configureSession(preferredDevice: CaptureDevice?)

    /**
     * When non-null, preview start must be blocked — user-visible explanation.
     */
    fun describeStartBlocker(
        context: Context,
        usbRepository: UsbCaptureDeviceRepository,
        device: CaptureDevice,
    ): String?

    /**
     * When [bindPreviewSurface] runs preview start on the same thread (e.g. TextureView already
     * available) and native open fails, implementations set a one-shot flag; callers should skip
     * entering a "running" capture state until this is consumed.
     */
    fun consumeLastPreviewStartFailed(): Boolean = false

    fun getSupportedSizes(): List<org.centennialoss.consolation.uvc.Size>
    fun probeSupportedSizes(device: CaptureDevice): List<org.centennialoss.consolation.uvc.Size>

    /**
     * When the last [probeSupportedSizes] returned an empty list because opening the UVC device
     * failed (stuck USB session), this returns true once and clears the latch.
     */
    fun consumeLastProbeOpenFailed(): Boolean = false
    fun setPreviewSize(width: Int, height: Int, fps: Int)
    fun setPreferredPixelFormat(frameFormat: Int?) = Unit

    /**
     * Linear gain 0f–1f for USB capture audio played through [UsbCaptureAudioLoop].
     * No-op when no audio track is active.
     */
    fun setCaptureAudioVolume(linear: Float) = Unit

    /**
     * Called when video starts but capture-card audio cannot be routed to the selected USB device.
     */
    fun setCaptureAudioFailureListener(listener: (() -> Unit)?) = Unit

    fun getTelemetry(): org.centennialoss.consolation.core.telemetry.TelemetrySnapshot

    /**
     * `true` once the first preview frame has been produced for the current open stream; `false`
     * during the initial black / pipeline startup. Used to avoid spurious low-FPS UI.
     */
    fun hasReceivedFirstVideoFrame(): Boolean = true

    fun setRotation(degrees: Int)
    fun setFlip(horizontal: Boolean, vertical: Boolean)

    fun dispose()

    /**
     * Detach the preview surface and wait until native preview teardown has drained.
     * Use from a background thread for explicit user Stop actions.
     */
    fun unbindPreviewSurfaceBlocking() {
        unbindPreviewSurface()
    }

    /**
     * Call when the USB capture device was removed during an active session so cached connections
     * can be abandoned immediately (before async preview teardown).
     */
    fun prepareForUsbRemoval() = Unit
}
