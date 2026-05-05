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
)

interface LatencyTelemetry {
    fun snapshot(): TelemetrySnapshot
}
