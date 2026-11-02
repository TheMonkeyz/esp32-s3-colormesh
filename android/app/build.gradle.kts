// The ColorMesh phone app. Version: APP_VERSION (CI: the tag, "v0.7.0-rc.1"), else `git describe`, else v0.0.0-dev.
// versionCode orders like the boards' version byte (mesh_proto.h): major, minor, patch, then x.N < rc.N < release.
// Release signing from the environment (CI secrets): CM_KEYSTORE (path), CM_KEYSTORE_PASS, CM_KEY_ALIAS, CM_KEY_PASS.
plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

fun appVersion(): String {
    System.getenv("APP_VERSION")?.takeIf { it.isNotBlank() }?.let { return it }
    return try {
        val p = ProcessBuilder("git", "describe", "--tags", "--always").directory(rootDir).start()
        p.inputStream.bufferedReader().readText().trim().ifEmpty { "v0.0.0-dev" }
    } catch (e: Exception) { "v0.0.0-dev" }
}

fun versionCodeOf(v: String): Int {
    val m = Regex("""v?(\d+)\.(\d+)\.(\d+)(?:-(.*))?""").matchEntire(v) ?: return 1
    val (a, b, c) = m.destructured
    val rest = m.groupValues[4]
    val kind = when {
        rest.isEmpty() -> 200
        rest.startsWith("rc.") -> 100 + (rest.removePrefix("rc.").takeWhile { it.isDigit() }.toIntOrNull() ?: 0).coerceAtMost(99)
        Regex("""\d+-g.*""").matches(rest) -> 201 + rest.takeWhile { it.isDigit() }.toInt().coerceAtMost(54)
        else -> (rest.substringAfterLast('.').toIntOrNull() ?: 0).coerceAtMost(99)
    }
    return ((a.toInt() * 100 + b.toInt()) * 100 + c.toInt()) * 1000 + kind
}

val ver = appVersion()

android {
    namespace = "ca.themonkeyz.colormesh"
    compileSdk = 35
    defaultConfig {
        applicationId = "ca.themonkeyz.colormesh"
        minSdk = 26
        targetSdk = 35
        versionName = ver.removePrefix("v")
        versionCode = versionCodeOf(ver)
    }
    signingConfigs {
        create("release") {
            val ks = System.getenv("CM_KEYSTORE")
            if (!ks.isNullOrBlank()) {
                storeFile = file(ks)
                storePassword = System.getenv("CM_KEYSTORE_PASS")
                keyAlias = System.getenv("CM_KEY_ALIAS")
                keyPassword = System.getenv("CM_KEY_PASS")
            }
        }
    }
    buildTypes {
        debug {
            applicationIdSuffix = ".debug"           // installs beside the release app
        }
        release {
            isMinifyEnabled = false
            if (!System.getenv("CM_KEYSTORE").isNullOrBlank()) signingConfig = signingConfigs.getByName("release")
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions { jvmTarget = "17" }
    testOptions { unitTests.isReturnDefaultValues = true }
    dependenciesInfo { includeInApk = false; includeInBundle = false }   // no dependency list in the APK
}

dependencies {
    compileOnly(project(":hidden"))                       // the hidden periodic sync API's shapes, never shipped
    implementation("org.lsposed.hiddenapibypass:hiddenapibypass:6.1")   // unlocks it (Hidden.kt)
    testImplementation("junit:junit:4.13.2")
}
