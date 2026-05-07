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
    /** Average compressed/frame payload size in KiB from native aggregation. */
    val nativePayloadAvgKb: Double = 0.0,
)

interface LatencyTelemetry {
    fun snapshot(): TelemetrySnapshot
}
