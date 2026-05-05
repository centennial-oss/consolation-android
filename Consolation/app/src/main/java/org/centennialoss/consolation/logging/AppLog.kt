package org.centennialoss.consolation.logging

import android.util.Log

/**
 * Centralized app logging policy.
 *
 * Build-time stripping for release is configured via R8/ProGuard rules:
 * `v/d/i` are removed from release bytecode; `w/e` are retained.
 */
object AppLog {
    fun v(tag: String, message: String): Int = Log.v(tag, message)

    fun v(tag: String, message: String, throwable: Throwable): Int = Log.v(tag, message, throwable)

    fun d(tag: String, message: String): Int = Log.d(tag, message)

    fun d(tag: String, message: String, throwable: Throwable): Int = Log.d(tag, message, throwable)

    fun i(tag: String, message: String): Int = Log.i(tag, message)

    fun i(tag: String, message: String, throwable: Throwable): Int = Log.i(tag, message, throwable)

    fun w(tag: String, message: String): Int = Log.w(tag, message)

    fun w(tag: String, message: String, throwable: Throwable): Int = Log.w(tag, message, throwable)

    fun w(tag: String, throwable: Throwable): Int = Log.w(tag, throwable)

    fun e(tag: String, message: String): Int = Log.e(tag, message)

    fun e(tag: String, message: String, throwable: Throwable): Int = Log.e(tag, message, throwable)
}
