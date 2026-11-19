package org.centennialoss.consolation.preview

import android.graphics.SurfaceTexture
import android.os.Build
import android.view.TextureView
import android.view.Window

/**
 * Java/Kotlin-side preview hints (public NDK lacks [ANativeWindow_setBufferCount]).
 * Reduces excess compositing work when the texture is used only for live video.
 */
object UsbPreviewSurfaceHints {

    fun TextureView.applyForUsbPreviewLatency() {
        isOpaque = false
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            setLayerType(TextureView.LAYER_TYPE_HARDWARE, null)
        }
    }

    /**
     * While live video is on screen, ask for the panel's fastest refresh rate at the
     * current resolution. Frames are presented as soon as they are decoded (swap
     * interval 0), so what is left is the wait for the next display latch: at 120 Hz
     * that is at most ~8 ms instead of ~16 ms, whatever the source frame rate.
     *
     * Deliberately not [android.view.Surface.setFrameRate]: declaring the content's own
     * rate (say 60 fps) invites the system to drop the panel to that rate, which is the
     * opposite of what low latency wants.
     *
     * Also sets [android.view.WindowManager.LayoutParams.preferMinimalPostProcessing],
     * which switches an HDMI-attached display into its low-latency (game) mode where
     * supported. Passing false restores the system defaults.
     */
    fun Window.applyLowLatencyDisplayMode(active: Boolean) {
        val display = decorView.display ?: return
        val wantedModeId = if (active) {
            val current = display.mode
            display.supportedModes
                .filter {
                    it.physicalWidth == current.physicalWidth &&
                        it.physicalHeight == current.physicalHeight
                }
                .maxByOrNull { it.refreshRate }
                ?.takeIf { it.refreshRate > current.refreshRate + REFRESH_RATE_EPSILON_HZ }
                ?.modeId
                ?: current.modeId
        } else {
            0
        }
        val attrs = attributes
        var changed = false
        if (attrs.preferredDisplayModeId != wantedModeId) {
            attrs.preferredDisplayModeId = wantedModeId
            changed = true
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R &&
            attrs.preferMinimalPostProcessing != active
        ) {
            attrs.preferMinimalPostProcessing = active
            changed = true
        }
        if (changed) {
            attributes = attrs
        }
    }

    private const val REFRESH_RATE_EPSILON_HZ = 0.5f

    fun SurfaceTexture.setDefaultBufferSizeIfValid(widthPx: Int, heightPx: Int) {
        if (widthPx > 1 && heightPx > 1) {
            setDefaultBufferSize(widthPx, heightPx)
        }
    }
}
