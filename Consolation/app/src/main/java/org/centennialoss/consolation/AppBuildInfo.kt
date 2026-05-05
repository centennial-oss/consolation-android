package org.centennialoss.consolation

object AppBuildInfo {
    const val version: String = "1.0.0"
    const val buildType: String = "Release"
    const val buildDate: String = "2026-05-05T12:11:07.000Z"
    const val commit: String = "d9578c2f16eb6fbac75af145afbb60487f51b31b"

    val copyableBlob: String
        get() = """
            Version: $version (Android)
            Build Type: $buildType
            Date: $buildDate
            Commit: $commit
        """.trimIndent()
}
