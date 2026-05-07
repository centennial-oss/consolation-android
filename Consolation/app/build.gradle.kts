import java.io.File
import java.util.Properties

plugins {
    alias(libs.plugins.android.application)
}

/** Prefer Consolation/keystore.properties, then repo-root keystore.properties (Gradle root is Consolation/). */
val keystorePropertiesFile: File =
    listOf(
        rootProject.file("keystore.properties"),
        rootProject.rootDir.parentFile.resolve("keystore.properties"),
    ).firstOrNull { it.exists() } ?: rootProject.file("keystore.properties")

val keystoreProperties = Properties()
if (keystorePropertiesFile.exists()) {
    keystorePropertiesFile.inputStream().use { keystoreProperties.load(it) }
}

/** Prefer Consolation/build.properties, then repo-root build.properties. */
val buildPropertiesFile: File =
    listOf(
        rootProject.file("build.properties"),
        rootProject.rootDir.parentFile.resolve("build.properties"),
    ).firstOrNull { it.exists() } ?: rootProject.file("build.properties")

val buildProperties = Properties()
if (buildPropertiesFile.exists()) {
    buildPropertiesFile.inputStream().use { buildProperties.load(it) }
}

val appVersionCode: Int =
    buildProperties.getProperty("consolation.build.number")?.toIntOrNull() ?: 1
val appVersionName: String =
    buildProperties.getProperty("consolation.build.version") ?: "1.0.0"

val nativeReleaseInDebug: Boolean =
    findProperty("consolation.uvccamera.nativeReleaseInDebug")?.toString()?.toBoolean() == true

/** Optional isoch transfer ring depth for bundled libuvc (default 10 when unset). Range 3…128. */
val libuvcNumTransferBufs: Int? =
    findProperty("consolation.libuvc.numTransferBuffers")?.toString()?.trim()?.toIntOrNull()
        ?.takeIf { candidate -> candidate in 3..128 }

android {
    namespace = "org.centennialoss.consolation"
    compileSdk {
        version = release(36) {
            minorApiLevel = 1
        }
    }

    defaultConfig {
        applicationId = "org.centennialoss.consolation"
        minSdk = libs.versions.minSdk.get().toInt()
        targetSdk = 36
        versionCode = appVersionCode
        versionName = appVersionName

        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"

        ndk {
            abiFilters += listOf("armeabi-v7a", "arm64-v8a")
        }

        externalNativeBuild {
            ndkBuild {
                libuvcNumTransferBufs?.let { count ->
                    arguments += "-DLIBUVC_NUM_TRANSFER_BUFS=$count"
                }
            }
        }
    }

    signingConfigs {
        create("release") {
            if (keystorePropertiesFile.exists()) {
                val storeFileProp = keystoreProperties["storeFile"] as String
                storeFile =
                    File(storeFileProp).let { path ->
                        if (path.isAbsolute) path else rootProject.file(storeFileProp)
                    }
                storePassword = keystoreProperties["storePassword"] as String
                keyAlias = keystoreProperties["keyAlias"] as String
                keyPassword = keystoreProperties["keyPassword"] as String
            }
        }
    }

    buildTypes {
        debug {
            if (nativeReleaseInDebug) {
                isJniDebuggable = false
                externalNativeBuild {
                    ndkBuild {
                        arguments += listOf("APP_OPTIM=release")
                    }
                }
            }
        }
        release {
            if (keystorePropertiesFile.exists()) {
                signingConfig = signingConfigs.getByName("release")
            }
            isMinifyEnabled = true
            isShrinkResources = true
            isDebuggable = false
            isJniDebuggable = false
            isProfileable = false
            /* R8 minify strips AppLog.v/d/i and matching android.util.Log calls (see proguard-rules.pro). */
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
            /* utilbase.h: strip LOGx for all JNI modules that include localdefines.h */
            externalNativeBuild {
                ndkBuild {
                    arguments += listOf("APP_CPPFLAGS+=-DAPP_NATIVE_LOG_SILENT")
                }
            }
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_11
        targetCompatibility = JavaVersion.VERSION_11
    }
    buildFeatures {
        viewBinding = true
        buildConfig = true
    }
    externalNativeBuild {
        ndkBuild {
            path = file("src/main/jni/Android.mk")
        }
    }
}

dependencies {
    implementation(libs.androidx.appcompat)
    implementation(libs.androidx.activity.ktx)
    implementation(libs.androidx.core.ktx)
    implementation(libs.androidx.lifecycle.runtime.ktx)
    implementation(libs.kotlinx.coroutines.android)
    implementation(libs.material)
    testImplementation(libs.junit)
    androidTestImplementation(libs.androidx.espresso.core)
    androidTestImplementation(libs.androidx.junit)
}
