package org.centennialoss.consolation.core.capture

import kotlinx.coroutines.flow.StateFlow

interface CaptureEngine {
    val state: StateFlow<CaptureState>
    suspend fun startWatching(device: CaptureDevice)
    suspend fun stopWatching()

    /**
     * Called when the USB device for the current session is no longer present.
     * Implementations should leave a non-running state suitable for [updateReadyState] (e.g. Ready).
     */
    fun abandonWatchSessionDueToUsbRemoval() = Unit
}
