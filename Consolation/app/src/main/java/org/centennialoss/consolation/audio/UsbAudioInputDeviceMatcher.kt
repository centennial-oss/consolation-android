package org.centennialoss.consolation.audio

import android.hardware.usb.UsbDevice
import android.media.AudioDeviceInfo
import android.media.AudioManager
import org.centennialoss.consolation.logging.AppLog as Log
import java.io.File
import java.util.Locale

/**
 * Picks the [AudioDeviceInfo] input that belongs to the same physical USB gadget as [usbDevice].
 * USB audio addresses expose ALSA card numbers (for example `card=2;device=0`). Android also
 * exposes the backing USB bus address in `/proc/asound/cardN/usbbus`, which lets us match the
 * audio input to [UsbDevice.deviceName] instead of guessing by product name.
 */
internal object UsbAudioInputDeviceMatcher {
    fun resolve(audioManager: AudioManager, usbDevice: UsbDevice): AudioDeviceInfo? {
        Log.i(logTag, "$logPrefix BEGIN video=${usbDevice.describeForLog()}")
        val inputs = audioManager.getDevices(AudioManager.GET_DEVICES_INPUTS)
            .filter { info ->
                info.isSource &&
                    (
                        info.type == AudioDeviceInfo.TYPE_USB_DEVICE ||
                            info.type == AudioDeviceInfo.TYPE_USB_HEADSET ||
                            info.type == AudioDeviceInfo.TYPE_USB_ACCESSORY
                        )
            }
        Log.i(logTag, "$logPrefix usb_audio_input_count=${inputs.size}")
        inputs.forEachIndexed { index, info ->
            Log.i(logTag, "$logPrefix input[$index] ${info.describeForLogWithAlsaMapping()}")
        }
        Log.i(logTag, "$logPrefix proc_asound ${describeProcAsoundSnapshot()}")
        if (inputs.isEmpty()) {
            Log.w(logTag, "$logPrefix END no_usb_audio_inputs video=${usbDevice.deviceName}")
            return null
        }

        val usbDeviceName = usbDevice.deviceName
        val alsaMatched = inputs.filter { info ->
            matchesUsbDeviceName(info, usbDeviceName)
        }
        if (alsaMatched.size == 1) {
            return alsaMatched.first().also { selected ->
                Log.i(
                    logTag,
                    "$logPrefix END selected_by_alsa_usb_bus video=$usbDeviceName " +
                        "audio=${selected.describeForLog()}",
                )
            }
        }
        if (alsaMatched.size > 1) {
            Log.w(
                logTag,
                "$logPrefix END ambiguous_multiple_alsa_matches video=$usbDeviceName " +
                    alsaMatched.joinToString { it.describeForLog() },
            )
            return null
        }

        val usbProduct = usbDevice.productName?.trim()?.toString()?.lowercase(Locale.ROOT)
        if (usbProduct.isNullOrEmpty()) {
            Log.w(
                logTag,
                "$logPrefix END ambiguous_no_video_product_name video=$usbDeviceName " +
                    "inputs=${inputs.joinToString { it.describeForLog() }}",
            )
            return null
        }

        val scored = inputs.map { info ->
            val audioName = info.productName?.trim()?.toString()?.lowercase(Locale.ROOT) ?: ""
            val score = when {
                audioName.isEmpty() -> 0
                audioName == usbProduct -> 4
                audioName.contains(usbProduct) || usbProduct.contains(audioName) -> 3
                else -> 0
            }
            info to score
        }
        val bestScore = scored.maxOf { it.second }
        val best = scored.filter { it.second == bestScore }
        if (bestScore > 0 && best.size == 1) {
            return best.first().first.also { selected ->
                Log.i(
                    logTag,
                    "$logPrefix END selected_by_unique_product_name video=$usbDeviceName " +
                        "product=$usbProduct audio=${selected.describeForLog()}",
                )
            }
        }

        Log.w(
            logTag,
            "$logPrefix END ambiguous_product_match video=$usbDeviceName product=$usbProduct; " +
                "inputs=${inputs.joinToString { it.describeForLog() }}",
        )
        return null
    }

    internal fun matchesUsbDeviceName(
        audioDevice: AudioDeviceInfo,
        usbDeviceName: String,
        procAsoundRoot: File = File("/proc/asound"),
    ): Boolean {
        if (audioDevice.address == usbDeviceName) {
            return true
        }
        val cardNumber = parseAlsaCardNumber(audioDevice.address) ?: return false
        return usbDeviceNameForAlsaCard(cardNumber, procAsoundRoot) == usbDeviceName
    }

    internal fun parseAlsaCardNumber(address: String): Int? {
        val cardPrefix = "card="
        val cardStart = address.indexOf(cardPrefix)
        if (cardStart < 0) {
            return null
        }
        val numberStart = cardStart + cardPrefix.length
        val numberEnd = address.indexOf(';', numberStart).takeIf { it >= 0 } ?: address.length
        return address.substring(numberStart, numberEnd).toIntOrNull()
    }

    internal fun usbDeviceNameForAlsaCard(cardNumber: Int, procAsoundRoot: File): String? {
        val usbbus = File(procAsoundRoot, "card$cardNumber/usbbus")
        return probeAlsaCardUsbDeviceName(cardNumber, procAsoundRoot).usbDeviceName
    }

    private fun probeAlsaCardUsbDeviceName(cardNumber: Int, procAsoundRoot: File): AlsaCardUsbProbe {
        val usbbus = File(procAsoundRoot, "card$cardNumber/usbbus")
        return runCatching {
            usbbus.readText().trim()
                .takeIf { it.isNotEmpty() }
                ?.let { "$usbDevicePrefix/$it" }
        }.fold(
            onSuccess = { usbDeviceName ->
                AlsaCardUsbProbe(
                    cardNumber = cardNumber,
                    usbbusPath = usbbus.absolutePath,
                    exists = usbbus.exists(),
                    canRead = usbbus.canRead(),
                    usbDeviceName = usbDeviceName,
                    error = null,
                )
            },
            onFailure = { error ->
                AlsaCardUsbProbe(
                    cardNumber = cardNumber,
                    usbbusPath = usbbus.absolutePath,
                    exists = usbbus.exists(),
                    canRead = usbbus.canRead(),
                    usbDeviceName = null,
                    error = error.javaClass.simpleName + ":" + (error.message ?: ""),
                )
            },
        )
    }

    private fun AudioDeviceInfo.describeForLog(): String =
        "id=$id type=$type name=${productName} address=$address"

    private fun AudioDeviceInfo.describeForLogWithAlsaMapping(): String {
        val cardNumber = parseAlsaCardNumber(address)
        val alsaProbe = cardNumber?.let { probeAlsaCardUsbDeviceName(it, File("/proc/asound")) }
        val sampleRateLabel = sampleRates.takeIf { it.isNotEmpty() }?.joinToString()
            ?: "any"
        val channelCountLabel = channelCounts.takeIf { it.isNotEmpty() }?.joinToString()
            ?: "unknown"
        return describeForLog() +
            " sampleRates=[$sampleRateLabel]" +
            " channelCounts=[$channelCountLabel]" +
            " alsaCard=${cardNumber ?: "none"}" +
            " mappedUsb=${alsaProbe?.usbDeviceName ?: "none"}" +
            " usbbusPath=${alsaProbe?.usbbusPath ?: "none"}" +
            " usbbusExists=${alsaProbe?.exists ?: false}" +
            " usbbusCanRead=${alsaProbe?.canRead ?: false}" +
            " usbbusError=${alsaProbe?.error ?: "none"}"
    }

    private fun describeProcAsoundSnapshot(procAsoundRoot: File = File("/proc/asound")): String {
        val cardsText = runCatching {
            File(procAsoundRoot, "cards").readLines()
                .take(MAX_PROC_ASOUND_CARDS_LINES)
                .joinToString(separator = " | ") { it.trim() }
        }.getOrElse { error ->
            "unreadable(${error.javaClass.simpleName}:${error.message ?: ""})"
        }
        val cardDirs = runCatching {
            procAsoundRoot.listFiles()
                ?.filter { it.isDirectory && it.name.matches(cardDirRegex) }
                ?.sortedBy { parseAlsaCardNumber("card=${it.name.removePrefix("card")}") ?: Int.MAX_VALUE }
                ?.joinToString(separator = "; ") { dir ->
                    val cardNumber = dir.name.removePrefix("card").toIntOrNull()
                    if (cardNumber == null) {
                        "${dir.name}:unparseable"
                    } else {
                        probeAlsaCardUsbDeviceName(cardNumber, procAsoundRoot).describeForSnapshot()
                    }
                }
                ?: "none"
        }.getOrElse { error ->
            "unreadable(${error.javaClass.simpleName}:${error.message ?: ""})"
        }
        return "cards=\"$cardsText\" cardDirs=\"$cardDirs\""
    }

    private fun UsbDevice.describeForLog(): String {
        val interfaces = (0 until interfaceCount).joinToString(prefix = "[", postfix = "]") { index ->
            val intf = getInterface(index)
            "#$index class=${intf.interfaceClass} subclass=${intf.interfaceSubclass} protocol=${intf.interfaceProtocol}"
        }
        return "deviceName=$deviceName vendorId=0x${vendorId.toString(16)} " +
            "productId=0x${productId.toString(16)} productName=${productName ?: "null"} " +
            "manufacturerName=${manufacturerName ?: "null"} deviceClass=$deviceClass " +
            "deviceSubclass=$deviceSubclass deviceProtocol=$deviceProtocol interfaces=$interfaces"
    }

    internal const val logTag = "ConsolationUsbAudio"
    private const val logPrefix = "usb_audio_route"
    private const val usbDevicePrefix = "/dev/bus/usb"
    private const val MAX_PROC_ASOUND_CARDS_LINES = 12
    private val cardDirRegex = Regex("""card\d+""")

    private data class AlsaCardUsbProbe(
        val cardNumber: Int,
        val usbbusPath: String,
        val exists: Boolean,
        val canRead: Boolean,
        val usbDeviceName: String?,
        val error: String?,
    ) {
        fun describeForSnapshot(): String =
            "card=$cardNumber path=$usbbusPath exists=$exists canRead=$canRead " +
                "mappedUsb=${usbDeviceName ?: "none"} error=${error ?: "none"}"
    }
}
