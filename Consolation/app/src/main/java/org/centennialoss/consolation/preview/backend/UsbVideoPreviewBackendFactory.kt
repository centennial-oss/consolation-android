package org.centennialoss.consolation.preview.backend

import android.content.Context
import org.centennialoss.consolation.usb.UsbCaptureDeviceRepository

object UsbVideoPreviewBackendFactory {
    fun create(
        context: Context,
        usbRepository: UsbCaptureDeviceRepository,
    ): UsbVideoPreviewBackend = UvccameraLibPreviewBackend(context, usbRepository)
}
