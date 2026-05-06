package org.centennialoss.consolation.capture

import org.centennialoss.consolation.core.capture.CaptureDevice
import java.util.Locale

data class DeviceCompatibilityDefaultFormat(
    val width: Int,
    val height: Int,
    val frameRate: Float,
)

data class DeviceCompatibilityIssue(
    val id: String,
    val vendorId: Int?,
    val productId: Int?,
    val manufacturer: String?,
    val model: String?,
    val summary: String,
    val defaultFormat: DeviceCompatibilityDefaultFormat? = null,
) {
    fun matches(device: CaptureDevice): Boolean =
        matchesIfProvided(vendorId, device.vendorId) &&
            matchesIfProvided(productId, device.productId) &&
            matchesIfProvided(manufacturer, device.name) &&
            matchesIfProvided(model, device.name, device.id)

    private fun matchesIfProvided(needle: Int?, value: Int): Boolean =
        needle == null || needle == value

    private fun matchesIfProvided(needle: String?, vararg haystacks: String): Boolean {
        if (needle.isNullOrBlank()) return true
        val normalizedNeedle = needle.lowercase(Locale.US)
        return haystacks.any { haystack ->
            haystack.lowercase(Locale.US).contains(normalizedNeedle)
        }
    }
}

object DeviceCompatibilityIssues {
    private val known = listOf(
        DeviceCompatibilityIssue(
            id = "android-macrosilicon-2109",
            vendorId = 0x534d,
            productId = 0x2109,
            manufacturer = null,
            model = null,
            summary = "MacroSilicon 2109 capture devices that have a max framerate of 30p at " +
                "1920x1080 may incorrectly advertise a 60p capability. If you choose 1920x1080 " + 
                "@ 60p, video may freeze or report that the card is only sending 30fps. If you " +
                "experience issues, use 1920x1080 @ 30p, or use 1280x720 for true 60p video.",
            defaultFormat = DeviceCompatibilityDefaultFormat(
                width = 1920,
                height = 1080,
                frameRate = 30f,
            ),
        ),
        DeviceCompatibilityIssue(
            id = "android-macrosilicon-2109-name-match",
            vendorId = null,
            productId = null,
            manufacturer = "macrosilicon",
            model = "2109",
            summary = "MacroSilicon 2109 capture devices that have a max framerate of 30p at " +
                "1920x1080 may incorrectly advertise a 60p capability. If you choose 1920x1080 " + 
                "@ 60p, video may freeze or report that the card is only sending 30fps. If you " +
                "experience issues, use 1920x1080 @ 30p, or use 1280x720 for true 60p video.",
            defaultFormat = DeviceCompatibilityDefaultFormat(
                width = 1920,
                height = 1080,
                frameRate = 30f,
            ),
        ),
    )

    fun issueFor(device: CaptureDevice?): DeviceCompatibilityIssue? {
        device ?: return null
        return known.firstOrNull { it.matches(device) }
    }
}
