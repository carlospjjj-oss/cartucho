plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

// Version from CI (run number) so every APK installs over the previous one.
val buildNumber = (System.getenv("GITHUB_RUN_NUMBER") ?: "1").toInt()

android {
    namespace = "com.cartucho.emu"
    compileSdk = 35
    ndkVersion = System.getenv("CARTUCHO_NDK_VERSION") ?: "29.0.14206865"

    defaultConfig {
        applicationId = "com.cartucho.emu"
        minSdk = 28
        targetSdk = 35
        versionCode = buildNumber
        versionName = "0.1.$buildNumber"
        ndk {
            abiFilters += listOf("arm64-v8a")
            // The screenshot workflow adds x86_64 to run on the CI emulator.
            (project.findProperty("extraAbi") as String?)?.let { abiFilters += it }
        }
        externalNativeBuild {
            cmake {
                arguments += listOf("-DANDROID_STL=c++_shared")
            }
        }
    }

    signingConfigs {
        // One fixed key so every new APK installs over the previous one and
        // keeps saves. A key in the CARTUCHO_KEYSTORE* secrets wins; else the
        // project key (fine for a personal build, not for publishing).
        create("cartucho") {
            val secret = System.getenv("CARTUCHO_KEYSTORE")?.let { file(it) }?.takeIf { it.exists() }
            storeFile = secret ?: file("cartucho.keystore")
            storePassword = if (secret != null) System.getenv("CARTUCHO_KEYSTORE_PASSWORD") else "cartucho"
            keyAlias = if (secret != null) (System.getenv("CARTUCHO_KEY_ALIAS") ?: "cartucho") else "cartucho"
            keyPassword = if (secret != null) System.getenv("CARTUCHO_KEYSTORE_PASSWORD") else "cartucho"
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            signingConfig = signingConfigs.getByName("cartucho")
        }
        debug {
            signingConfig = signingConfigs.getByName("cartucho")
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    packaging {
        jniLibs {
            // The cores are dlopen()ed by path; keep them extracted.
            useLegacyPackaging = true
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions {
        jvmTarget = "17"
    }
    buildFeatures {
        buildConfig = true
    }
}

dependencies {
    implementation("androidx.core:core-ktx:1.13.1")
    implementation("androidx.appcompat:appcompat:1.7.0")
    implementation("androidx.activity:activity-ktx:1.9.3")
    implementation("androidx.recyclerview:recyclerview:1.3.2")
    implementation("androidx.documentfile:documentfile:1.0.1")
    implementation("androidx.preference:preference-ktx:1.2.1")
    implementation("com.google.android.material:material:1.12.0")
    // Games in .7z files (LZMA/LZMA2 need xz).
    implementation("org.apache.commons:commons-compress:1.27.1")
    implementation("org.tukaani:xz:1.10")
}
