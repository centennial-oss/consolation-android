package org.centennialoss.consolation.core.capture

data class CaptureDevice(
    val id: String,
    val name: String,
    val displayName: String = name,
    val vendorId: Int,
    val productId: Int,
)
