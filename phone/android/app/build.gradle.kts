plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "com.csheets.camlink"
    compileSdk = 34

    defaultConfig {
        applicationId = "com.csheets.camlink"
        minSdk = 29          // WifiNetworkSpecifier требует Android 10+
        targetSdk = 34
        versionCode = 1
        versionName = "1.0"
    }

    buildTypes {
        release {
            isMinifyEnabled = false
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions {
        jvmTarget = "17"
    }
}

// Внешних зависимостей нет — приложение полностью на системных API
dependencies { }
