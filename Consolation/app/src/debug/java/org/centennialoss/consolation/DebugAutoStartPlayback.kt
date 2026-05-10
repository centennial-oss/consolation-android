package org.centennialoss.consolation

/**
 * Debug-only automation flag for launch smoke tests.
 *
 * Set to 1 locally to auto-enter playback after resolution probing selects a format.
 * Release builds define the same constant in src/release and force it to 0.
 */
internal const val AUTO_START_PLAYBACK = 1
