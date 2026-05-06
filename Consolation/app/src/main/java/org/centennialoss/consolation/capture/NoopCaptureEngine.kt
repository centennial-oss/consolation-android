package org.centennialoss.consolation.capture

import org.centennialoss.consolation.core.capture.CaptureDevice
import org.centennialoss.consolation.core.capture.CaptureEngine
import org.centennialoss.consolation.core.capture.CaptureState
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow

class NoopCaptureEngine : CaptureEngine {
    private val mutableState = MutableStateFlow<CaptureState>(CaptureState.Idle)
    override val state: StateFlow<CaptureState> = mutableState.asStateFlow()

    override suspend fun startWatching(device: CaptureDevice) {
        mutableState.value = CaptureState.Running
    }

    override suspend fun stopWatching() {
        mutableState.value = CaptureState.Ready
    }

    override fun abandonWatchSessionDueToUsbRemoval() {
        if (mutableState.value is CaptureState.Running) {
            mutableState.value = CaptureState.Ready
        }
    }

    fun updateReadyState(hasDevices: Boolean) {
        if (mutableState.value is CaptureState.Running) {
            return
        }
        mutableState.value = if (hasDevices) CaptureState.Ready else CaptureState.NoDevice
    }

    fun setRequestingPermission() {
        if (mutableState.value !is CaptureState.Running) {
            mutableState.value = CaptureState.RequestingPermission
        }
    }

    fun setFailed(message: String) {
        mutableState.value = CaptureState.Failed(message)
    }
}
