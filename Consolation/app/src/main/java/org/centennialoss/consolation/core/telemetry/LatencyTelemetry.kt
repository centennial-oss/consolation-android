package org.centennialoss.consolation.core.telemetry

data class TelemetrySnapshot(
    val fps: Int,
    val droppedFrames: Int,
    val stutter: Double,
    val width: Int,
    val height: Int,
    val configuredFps: Int,
    val pixelFormat: String,
    val backendName: String,
    val nativePreviewConversionCount: Long = 0,
    val nativePreviewConversionAvgMs: Double = 0.0,
    val nativePreviewConversionMaxMs: Double = 0.0,
    val nativeCallbackConversionCount: Long = 0,
    val nativeCallbackConversionAvgMs: Double = 0.0,
    val nativeCallbackConversionMaxMs: Double = 0.0,
    val nativeSurfaceCopyCount: Long = 0,
    val nativeSurfaceCopyAvgMs: Double = 0.0,
    val nativeSurfaceCopyMaxMs: Double = 0.0,
    val nativePayloadCount: Long = 0,
    val nativePayloadAvgKb: Double = 0.0,
    val nativePayloadMaxKb: Double = 0.0,
)

interface LatencyTelemetry {
    fun snapshot(): TelemetrySnapshot
}
