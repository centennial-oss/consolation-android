package org.centennialoss.consolation.core.telemetry

data class TelemetrySnapshot(
    val fps: Int,
    val droppedFrames: Int,
    val width: Int,
    val height: Int,
    val configuredFps: Int,
    val pixelFormat: String,
    val backendName: String,
    val nativeEndToEndLatencyAvgMs: Double = 0.0,
    val nativeQueuedAvgFrames: Double = 0.0,
    /** Average compressed/frame payload size in KiB from native aggregation. */
    val nativePayloadAvgKb: Double = 0.0,
    val nativePreviewConvAvgMs: Double = 0.0,
    val nativeEndToEndMaxMs: Double = 0.0,
    val nativeQueueDeqMaxFrames: Double = 0.0,
    val nativeQueueEnqAvgFrames: Double = 0.0,
    val nativeQueueEnqMaxFrames: Double = 0.0,
    val nativeUvcCbAvgMs: Double = 0.0,
    val nativeUvcCbMaxMs: Double = 0.0,
    val nativeCbLagAvgMs: Double = 0.0,
    val nativeCbLagMaxMs: Double = 0.0,
    val nativeCbLagCount: Long = 0L,
    val nativePubFps: Double = 0.0,
    val nativePreCbSkip: Long = 0L,
    val nativeStreamDrop: Long = 0L,
    val nativeFrameInterval100ns: Long = 0L,
    val nativeAltSetting: Long = 0L,
)

interface LatencyTelemetry {
    fun snapshot(): TelemetrySnapshot
}
