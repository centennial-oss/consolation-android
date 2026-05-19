package org.centennialoss.consolation

/**
 * Debug-only automation flag for launch smoke tests.
 *
 * Set to 1 locally to auto-enter playback after resolution probing selects a format.
 * Release builds define the same constant in src/release and force it to 0.
 */
internal const val AUTO_START_PLAYBACK = 0

/**
 * Debug-only native UVC/MJPEG diagnostic switch.
 *
 * Set to 1 locally to compile the native UVC runtime diagnostics into debug builds.
 * This is intentionally off by default because it adds hot-path logging and hashing.
 */
internal const val ENABLE_UVC_RUNTIME_DIAG = 0
