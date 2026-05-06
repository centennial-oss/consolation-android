package org.centennialoss.consolation.telemetry

import org.centennialoss.consolation.core.telemetry.LatencyTelemetry
import org.centennialoss.consolation.core.telemetry.TelemetrySnapshot

class DebugLatencyTelemetry : LatencyTelemetry {
    override fun snapshot(): TelemetrySnapshot {
        return TelemetrySnapshot(
            fps = 0,
            droppedFrames = 0,
            width = 0,
            height = 0,
            configuredFps = 0,
            pixelFormat = "",
            backendName = "noop-backend",
        )
    }
}
