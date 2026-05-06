package org.centennialoss.consolation.preview

import android.graphics.SurfaceTexture
import android.os.Build
import android.view.TextureView

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

    fun SurfaceTexture.setDefaultBufferSizeIfValid(widthPx: Int, heightPx: Int) {
        if (widthPx > 1 && heightPx > 1) {
            setDefaultBufferSize(widthPx, heightPx)
        }
    }
}
