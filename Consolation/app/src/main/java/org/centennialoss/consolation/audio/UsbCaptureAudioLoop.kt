package org.centennialoss.consolation.audio

import android.Manifest
import android.content.Context
import android.content.pm.PackageManager
import android.hardware.usb.UsbDevice
import android.os.SystemClock
import android.media.AudioAttributes
import android.media.AudioDeviceInfo
import android.media.AudioFormat
import android.media.AudioManager
import android.media.AudioRecord
import android.media.AudioTrack
import android.media.MediaRecorder
import androidx.core.content.ContextCompat
import org.centennialoss.consolation.logging.AppLog as Log
import java.nio.ByteBuffer
import java.util.concurrent.ArrayBlockingQueue
import java.util.concurrent.atomic.AtomicBoolean

/**
 * Captures PCM from the USB gadget's audio interface and plays it on the default playback device
 * (typically the built-in speaker when no wired headset is active).
 *
 * Uses a bounded queue between record and playback threads (default depth 2, tuned for low latency).
 */
class UsbCaptureAudioLoop(
    context: Context,
    private val queueDepth: Int = DEFAULT_QUEUE_DEPTH,
) {
    private val appContext = context.applicationContext
    private val audioManager = appContext.getSystemService(Context.AUDIO_SERVICE) as AudioManager

    private val running = AtomicBoolean(false)
    private var recordThread: Thread? = null
    private var playThread: Thread? = null
    private var audioRecord: AudioRecord? = null
    private var audioTrack: AudioTrack? = null
    private var pcmQueue: PcmChunkQueue? = null

    private var volumeLinear: Float = 1f

    /**
     * Starts capture/playback for [usbDevice]. Returns false if [Manifest.permission.RECORD_AUDIO]
     * is missing, no USB input is found, or hardware cannot be opened (video may still run).
     */
    @Synchronized
    fun start(usbDevice: UsbDevice): Boolean {
        Log.i(
            routeLogTag,
            "$routeLogPrefix audio_loop_start requested videoDeviceName=${usbDevice.deviceName} " +
                "videoProduct=${usbDevice.productName ?: "null"}",
        )
        if (running.get()) {
            Log.i(routeLogTag, "$routeLogPrefix audio_loop_start skipped_already_running")
            return true
        }
        if (ContextCompat.checkSelfPermission(appContext, Manifest.permission.RECORD_AUDIO) !=
            PackageManager.PERMISSION_GRANTED
        ) {
            Log.i(logTag, "RECORD_AUDIO not granted; skipping USB capture audio.")
            Log.w(routeLogTag, "$routeLogPrefix audio_loop_start failed_missing_record_audio_permission")
            return false
        }
        val inputDevice = UsbAudioInputDeviceMatcher.resolve(audioManager, usbDevice) ?: run {
            Log.i(logTag, "No USB audio input for ${usbDevice.deviceName}; skipping capture audio.")
            Log.w(
                routeLogTag,
                "$routeLogPrefix audio_loop_start failed_no_matched_input videoDeviceName=${usbDevice.deviceName}",
            )
            return false
        }
        Log.i(
            routeLogTag,
            "$routeLogPrefix audio_loop_start matched_input ${inputDevice.describeForRouteLog()}",
        )
        val sampleRate = pickSampleRate(inputDevice)
        val (inMask, channelCount) = pickInputChannelMask(inputDevice)
        val outMask = if (channelCount >= 2) {
            AudioFormat.CHANNEL_OUT_STEREO
        } else {
            AudioFormat.CHANNEL_OUT_MONO
        }

        val minRecord = AudioRecord.getMinBufferSize(sampleRate, inMask, AudioFormat.ENCODING_PCM_16BIT)
        if (minRecord <= 0) {
            Log.w(logTag, "Invalid AudioRecord min buffer for rate=$sampleRate inMask=$inMask")
            Log.w(
                routeLogTag,
                "$routeLogPrefix audio_loop_start failed_invalid_record_min_buffer " +
                    "rate=$sampleRate inMask=$inMask minRecord=$minRecord",
            )
            return false
        }
        val minPlay = AudioTrack.getMinBufferSize(sampleRate, outMask, AudioFormat.ENCODING_PCM_16BIT)
        if (minPlay <= 0) {
            Log.w(logTag, "Invalid AudioTrack min buffer for rate=$sampleRate outMask=$outMask")
            Log.w(
                routeLogTag,
                "$routeLogPrefix audio_loop_start failed_invalid_track_min_buffer " +
                    "rate=$sampleRate outMask=$outMask minPlay=$minPlay",
            )
            return false
        }

        val bytesPerFrameIn = channelCount * BYTES_PER_SAMPLE
        val chunkBytes = (sampleRate / CHUNKS_PER_SECOND).coerceAtLeast(32) * bytesPerFrameIn
        val recordBufferSize = (minRecord * 2).coerceAtLeast(chunkBytes * RECORD_RING_CHUNKS)

        val inputCapture: AudioRecord? = try {
            AudioRecord.Builder()
                .setContext(appContext)
                .setAudioSource(MediaRecorder.AudioSource.DEFAULT)
                .setAudioFormat(
                    AudioFormat.Builder()
                        .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                        .setSampleRate(sampleRate)
                        .setChannelMask(inMask)
                        .build(),
                )
                .setBufferSizeInBytes(recordBufferSize)
                .build()
        } catch (e: UnsupportedOperationException) {
            Log.w(logTag, "AudioRecord.Builder rejected parameters", e)
            null
        } catch (e: IllegalArgumentException) {
            Log.w(logTag, "AudioRecord open failed", e)
            null
        }
        if (inputCapture == null || inputCapture.state != AudioRecord.STATE_INITIALIZED) {
            inputCapture?.release()
            Log.w(logTag, "AudioRecord not initialized for USB input.")
            Log.w(routeLogTag, "$routeLogPrefix audio_loop_start failed_audiorecord_not_initialized")
            return false
        }
        @Suppress("DEPRECATION")
        val routed = inputCapture.setPreferredDevice(inputDevice)
        Log.i(
            routeLogTag,
            "$routeLogPrefix setPreferredDevice result=$routed preferred=${inputDevice.describeForRouteLog()}",
        )
        if (!routed) {
            Log.w(logTag, "Could not route AudioRecord to USB input; capture may use wrong microphone.")
        }

        val trackBufferSize = minPlay * 2
        val attributes = AudioAttributes.Builder()
            .setUsage(AudioAttributes.USAGE_MEDIA)
            .setContentType(AudioAttributes.CONTENT_TYPE_MOVIE)
            .build()
        val outFormat = AudioFormat.Builder()
            .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
            .setSampleRate(sampleRate)
            .setChannelMask(outMask)
            .build()
        val audioTrackBuilt = AudioTrack.Builder()
            .setAudioAttributes(attributes)
            .setAudioFormat(outFormat)
            .setBufferSizeInBytes(trackBufferSize)
            .setTransferMode(AudioTrack.MODE_STREAM)
            .setPerformanceMode(AudioTrack.PERFORMANCE_MODE_LOW_LATENCY)
            .build()
        val track = audioTrackBuilt
        if (track.state != AudioTrack.STATE_INITIALIZED) {
            track.release()
            inputCapture.release()
            Log.w(logTag, "AudioTrack not initialized.")
            Log.w(routeLogTag, "$routeLogPrefix audio_loop_start failed_audiotrack_not_initialized")
            return false
        }

        audioRecord = inputCapture
        audioTrack = track
        val queue = PcmChunkQueue(queueDepth)
        val pcmBufferPool = ArrayBlockingQueue<PcmChunk>(queueDepth + PCM_POOL_EXTRA_BUFFERS)
        repeat(queueDepth + PCM_POOL_EXTRA_BUFFERS) {
            pcmBufferPool.add(PcmChunk(ByteBuffer.allocateDirect(chunkBytes)))
        }
        pcmQueue = queue
        running.set(true)

        playThread = Thread({ playLoop(track, queue, pcmBufferPool) }, "usb-cap-audio-play").also {
            it.start()
        }
        recordThread = Thread(
            { recordLoop(inputCapture, queue, pcmBufferPool, chunkBytes) },
            "usb-cap-audio-rec",
        ).also { it.start() }

        track.play()
        applyVolumeToTrack(track)
        val chLabel = if (channelCount >= 2) "stereo" else "mono"
        Log.i(
            logTag,
            "USB capture audio started: rate=$sampleRate chIn=$chLabel inMask=$inMask " +
                "queueDepth=$queueDepth chunkBytes=$chunkBytes",
        )
        Log.i(
            routeLogTag,
            "$routeLogPrefix audio_loop_start success rate=$sampleRate chIn=$chLabel inMask=$inMask " +
                "outMask=$outMask minRecord=$minRecord minPlay=$minPlay recordBufferSize=$recordBufferSize " +
                "trackBufferSize=$trackBufferSize chunkBytes=$chunkBytes queueDepth=$queueDepth",
        )
        logOutputAudioPropertiesForLogcat(track, sampleRate)
        return true
    }

    @Synchronized
    fun stop() {
        val t0 = SystemClock.elapsedRealtime()
        Log.i(logTag, "stop enter thread=${Thread.currentThread().name}")
        if (!running.getAndSet(false)) {
            Log.i(logTag, "stop skip (already stopped)")
            return
        }
        pcmQueue?.clear()
        recordThread?.interrupt()
        playThread?.interrupt()
        runCatching { audioRecord?.stop() }
        Log.i(logTag, "stop after audioRecord.stop offsetMs=${SystemClock.elapsedRealtime() - t0}")
        recordThread.joinSafely("record")
        runCatching { audioTrack?.stop() }
        Log.i(logTag, "stop after audioTrack.stop offsetMs=${SystemClock.elapsedRealtime() - t0}")
        playThread.joinSafely("play")
        recordThread = null
        playThread = null
        pcmQueue = null
        audioRecord?.release()
        audioTrack?.release()
        audioRecord = null
        audioTrack = null
        Log.i(logTag, "USB capture audio stopped totalMs=${SystemClock.elapsedRealtime() - t0}")
    }

    @Synchronized
    fun setVolume(linear: Float) {
        volumeLinear = linear.coerceIn(0f, 1f)
        audioTrack?.let { applyVolumeToTrack(it) }
    }

    private fun applyVolumeToTrack(track: AudioTrack) {
        runCatching { track.setVolume(volumeLinear) }
    }

    private fun recordLoop(
        inputCapture: AudioRecord,
        queue: PcmChunkQueue,
        pcmBufferPool: ArrayBlockingQueue<PcmChunk>,
        chunkBytes: Int,
    ) {
        try {
            inputCapture.startRecording()
            Log.i(
                routeLogTag,
                "$routeLogPrefix AudioRecord.startRecording routed=${inputCapture.routedDevice?.describeForRouteLog() ?: "null"} " +
                    "preferred=${inputCapture.preferredDevice?.describeForRouteLog() ?: "null"} " +
                    "recordingState=${inputCapture.recordingState}",
            )
            while (running.get()) {
                val chunk = acquirePcmBuffer(queue, pcmBufferPool) ?: break
                val buffer = chunk.buffer
                buffer.clear()
                buffer.limit(chunkBytes)
                var filled = 0
                while (filled < chunkBytes && running.get()) {
                    buffer.position(filled)
                    val n = inputCapture.read(buffer, chunkBytes - filled)
                    if (n < 0) {
                        Log.w(logTag, "AudioRecord.read error=$n")
                        recyclePcmBuffer(pcmBufferPool, chunk)
                        running.set(false)
                        return
                    }
                    if (n == 0) {
                        continue
                    }
                    filled += n
                }
                chunk.sizeBytes = filled
                if (!running.get()) {
                    recyclePcmBuffer(pcmBufferPool, chunk)
                    break
                }
                queue.offerDroppingOldest(chunk)?.let {
                    recyclePcmBuffer(pcmBufferPool, it)
                    Log.v(logTag, "Audio queue full; dropped oldest chunk to maintain sync.")
                }
            }
        } catch (e: Exception) {
            Log.w(logTag, "Record loop failed", e)
        } finally {
            running.set(false)
            runCatching { inputCapture.stop() }
        }
    }

    private fun playLoop(
        track: AudioTrack,
        queue: PcmChunkQueue,
        pcmBufferPool: ArrayBlockingQueue<PcmChunk>,
    ) {
        try {
            while (true) {
                val chunk = try {
                    queue.poll(POLL_TIMEOUT_MS)
                } catch (_: InterruptedException) {
                    if (!running.get() && queue.isEmpty) break
                    continue
                }
                if (chunk == null) {
                    if (!running.get() && queue.isEmpty) break
                    continue
                }
                val buffer = chunk.buffer
                buffer.position(0)
                buffer.limit(chunk.sizeBytes)
                try {
                    while (buffer.hasRemaining()) {
                        val w = track.write(buffer, buffer.remaining(), AudioTrack.WRITE_BLOCKING)
                        if (w < 0) {
                            Log.w(logTag, "AudioTrack.write error=$w")
                            running.set(false)
                            return
                        }
                    }
                } finally {
                    recyclePcmBuffer(pcmBufferPool, chunk)
                }
            }
        } catch (e: Exception) {
            Log.w(logTag, "Play loop failed", e)
        }
    }

    private fun acquirePcmBuffer(
        queue: PcmChunkQueue,
        pcmBufferPool: ArrayBlockingQueue<PcmChunk>,
    ): PcmChunk? =
        pcmBufferPool.poll()
            ?: queue.pollNow()?.also {
                Log.v(logTag, "Audio buffer pool empty; dropped oldest queued chunk to reuse buffer.")
            }
            ?: run {
                Log.w(logTag, "Audio buffer pool exhausted; dropping capture chunk.")
                null
            }

    private fun recyclePcmBuffer(pcmBufferPool: ArrayBlockingQueue<PcmChunk>, chunk: PcmChunk) {
        chunk.sizeBytes = 0
        chunk.buffer.clear()
        if (!pcmBufferPool.offer(chunk)) {
            Log.w(logTag, "Audio buffer pool full while recycling chunk.")
        }
    }

    private class PcmChunk(val buffer: ByteBuffer) {
        var sizeBytes: Int = 0
    }

    private class PcmChunkQueue(private val capacity: Int) {
        private val lock = Object()
        private val chunks = ArrayDeque<PcmChunk>(capacity)

        val isEmpty: Boolean
            get() = synchronized(lock) { chunks.isEmpty() }

        fun offerDroppingOldest(chunk: PcmChunk): PcmChunk? =
            synchronized(lock) {
                val dropped = if (chunks.size >= capacity) chunks.removeFirst() else null
                chunks.addLast(chunk)
                lock.notify()
                dropped
            }

        fun pollNow(): PcmChunk? =
            synchronized(lock) {
                chunks.removeFirstOrNull()
            }

        @Throws(InterruptedException::class)
        fun poll(timeoutMs: Long): PcmChunk? =
            synchronized(lock) {
                if (chunks.isEmpty()) {
                    lock.wait(timeoutMs)
                }
                chunks.removeFirstOrNull()
            }

        fun clear() {
            synchronized(lock) {
                chunks.clear()
                lock.notifyAll()
            }
        }
    }

    private fun pickSampleRate(device: AudioDeviceInfo): Int {
        val rates = device.sampleRates
        if (rates.isNotEmpty()) {
            preferredRates.forEach { preferred ->
                if (rates.contains(preferred)) {
                    return preferred
                }
            }
            return rates.maxOrNull() ?: DEFAULT_SAMPLE_RATE
        }
        return DEFAULT_SAMPLE_RATE
    }

    private fun pickInputChannelMask(device: AudioDeviceInfo): Pair<Int, Int> {
        val counts = device.channelCounts
        if (counts.isNotEmpty()) {
            if (counts.contains(2)) {
                return AudioFormat.CHANNEL_IN_STEREO to 2
            }
            if (counts.contains(1)) {
                return AudioFormat.CHANNEL_IN_MONO to 1
            }
        }
        return AudioFormat.CHANNEL_IN_STEREO to 2
    }

    private fun AudioDeviceInfo.describeForRouteLog(): String {
        val sampleRateLabel = sampleRates.takeIf { it.isNotEmpty() }?.joinToString() ?: "any"
        val channelCountLabel = channelCounts.takeIf { it.isNotEmpty() }?.joinToString() ?: "unknown"
        return "id=$id type=$type name=${productName} address=$address " +
            "sampleRates=[$sampleRateLabel] channelCounts=[$channelCountLabel]"
    }

    private fun Thread?.joinSafely(label: String) {
        if (this == null) return
        try {
            join(JOIN_TIMEOUT_MS)
            if (isAlive) {
                Log.w(
                    logTag,
                    "join $label still alive after ${JOIN_TIMEOUT_MS}ms (possible blocking read/write)",
                )
            }
        } catch (_: InterruptedException) {
            interrupt()
        }
    }

    /**
     * One line for Logcat; grep for `usb_cap_audio_latency` or tag `UsbCapAudio`.
     *
     * [AudioManager.getProperty] for `OUTPUT_LATENCY_MS` is often **null** (OEMs omit it); we also
     * log AudioTrack buffer duration; [AudioManager] OUTPUT_LATENCY_MS is often omitted by OEMs.
     */
    private fun logOutputAudioPropertiesForLogcat(track: AudioTrack, playbackSampleRateHz: Int) {
        // String keys match android.media.AudioManager public property constants (API 17+).
        val propertyLatencyMs = audioManager.getProperty("android.media.property.OUTPUT_LATENCY_MS")
        val globalSampleRate = audioManager.getProperty("android.media.property.OUTPUT_SAMPLE_RATE")
        val framesPerBuffer = audioManager.getProperty("android.media.property.OUTPUT_FRAMES_PER_BUFFER")
        val bufferFrames = track.bufferSizeInFrames
        val bufferDurationMs =
            if (playbackSampleRateHz > 0 && bufferFrames > 0) {
                (bufferFrames * 1000L) / playbackSampleRateHz
            } else {
                null
            }
        val globalHz = globalSampleRate?.toIntOrNull() ?: playbackSampleRateHz
        val framesPerBufInt = framesPerBuffer?.toIntOrNull()
        val mixerPeriodMs =
            if (globalHz > 0 && framesPerBufInt != null && framesPerBufInt > 0) {
                (framesPerBufInt * 1000L) / globalHz
            } else {
                null
            }
        Log.i(
            logTag,
            "usb_cap_audio_latency OUTPUT_LATENCY_MS=$propertyLatencyMs " +
                "(null is common — OEMs often omit this property) " +
                "OUTPUT_SAMPLE_RATE=$globalSampleRate OUTPUT_FRAMES_PER_BUFFER=$framesPerBuffer " +
                "mixerPeriodMs=$mixerPeriodMs " +
                "AudioTrack_bufferFrames=$bufferFrames " +
                "AudioTrack_bufferDurationMs=$bufferDurationMs",
        )
    }

    companion object {
        private const val logTag = "UsbCapAudio"
        private const val routeLogTag = UsbAudioInputDeviceMatcher.logTag
        private const val routeLogPrefix = "usb_audio_route"
        private const val BYTES_PER_SAMPLE: Int = 2
        private const val RECORD_RING_CHUNKS: Int = 4
        private const val PCM_POOL_EXTRA_BUFFERS: Int = 2
        private const val CHUNKS_PER_SECOND: Int = 100
        private const val DEFAULT_SAMPLE_RATE: Int = 48_000
        private val preferredRates: IntArray = intArrayOf(48_000, 44_100, 96_000, 32_000)
        private const val POLL_TIMEOUT_MS: Long = 60L
        private const val JOIN_TIMEOUT_MS: Long = 2500L
        private const val DEFAULT_QUEUE_DEPTH: Int = 4
    }
}
