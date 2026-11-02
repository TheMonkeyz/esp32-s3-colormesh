// Compile-time stubs of Android's hidden periodic-advertising sync types (android.bluetooth.le.IPeriodicAdvertising
// Callback, PeriodicAdvertisingReport): the real classes live in the phone's Bluetooth module; the app compiles
// against these and never ships them (compileOnly). Hidden.kt reaches the service with HiddenApiBypass.
plugins {
    id("com.android.library")
}

android {
    namespace = "ca.themonkeyz.colormesh.hidden"
    compileSdk = 35
    defaultConfig { minSdk = 26 }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}
