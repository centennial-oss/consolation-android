package org.centennialoss.consolation.core.capture

sealed interface CaptureState {
    data object Idle : CaptureState
    data object RequestingPermission : CaptureState
    data object NoDevice : CaptureState
    data object Ready : CaptureState
    data object Running : CaptureState
    data class Failed(val message: String) : CaptureState
}
