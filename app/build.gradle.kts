plugins {
    alias(libs.plugins.android.application)
}

android {
    namespace = "com.example.playhubtv"
    compileSdk = 35

    defaultConfig {
        applicationId = "com.example.playhubtv"
        minSdk = 23
        targetSdk = 30
        versionCode = 1
        versionName = "1.0"

    }

    splits {
        abi {
            isEnable = true
            reset()
            include("armeabi-v7a")
            isUniversalApk = false
        }
    }

    signingConfigs {
        getByName("debug") {
            enableV1Signing = true
            enableV2Signing = true
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_11
        targetCompatibility = JavaVersion.VERSION_11
    }
}

dependencies {
    implementation(libs.libvlc.all)
    implementation(libs.nanohttpd)
    implementation(libs.zxing.core)
}
