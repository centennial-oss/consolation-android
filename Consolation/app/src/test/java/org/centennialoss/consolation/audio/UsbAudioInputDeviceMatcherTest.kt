package org.centennialoss.consolation.audio

import java.io.File
import kotlin.io.path.createTempDirectory
import kotlin.io.path.writeText
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class UsbAudioInputDeviceMatcherTest {
    @Test
    fun parseAlsaCardNumber_readsCardFromUsbAddress() {
        assertEquals(2, UsbAudioInputDeviceMatcher.parseAlsaCardNumber("card=2;device=0"))
        assertEquals(14, UsbAudioInputDeviceMatcher.parseAlsaCardNumber("card=14"))
    }

    @Test
    fun parseAlsaCardNumber_ignoresNonAlsaAddresses() {
        assertNull(UsbAudioInputDeviceMatcher.parseAlsaCardNumber(""))
        assertNull(UsbAudioInputDeviceMatcher.parseAlsaCardNumber("/dev/bus/usb/001/002"))
        assertNull(UsbAudioInputDeviceMatcher.parseAlsaCardNumber("card=abc;device=0"))
    }

    @Test
    fun usbDeviceNameForAlsaCard_readsUsbBusMapping() {
        val rootPath = createTempDirectory(prefix = "asound")
        val root = rootPath.toFile()
        try {
            val cardDir = File(root, "card3").also { it.mkdirs() }
            File(cardDir, "usbbus").toPath().writeText("001/042\n")

            assertEquals(
                "/dev/bus/usb/001/042",
                UsbAudioInputDeviceMatcher.usbDeviceNameForAlsaCard(3, root),
            )
        } finally {
            root.deleteRecursively()
        }
    }
}
