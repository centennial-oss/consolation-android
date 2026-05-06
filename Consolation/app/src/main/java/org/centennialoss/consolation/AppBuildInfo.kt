package org.centennialoss.consolation

object AppBuildInfo {
    const val version: String = "localdev"
    const val buildType: String = "Debug"
    const val buildDate: String = "localdev"
    const val commit: String = "localdev"

    val copyableBlob: String
        get() = """
            Version: $version (Android)
            Build Type: $buildType
            Date: $buildDate
            Commit: $commit
        """.trimIndent()
}
