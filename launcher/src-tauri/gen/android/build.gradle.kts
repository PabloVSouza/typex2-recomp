buildscript {
    repositories {
        google()
        mavenCentral()
    }
    dependencies {
        classpath("com.android.tools.build:gradle:9.3.1")
        classpath("org.jetbrains.kotlin:kotlin-gradle-plugin:2.2.10")
    }
}

allprojects {
    repositories {
        google()
        mavenCentral()
    }
}

tasks.register("clean").configure {
    delete("build")
}


// typex2-recomp: Tauri rewrites app/build.gradle.kts on every build, so the app's own
// settings live here.
subprojects {
    plugins.withId("com.android.application") {
        extensions.configure<com.android.build.api.dsl.ApplicationExtension>("android") {
            // debug builds install next to the release app while developing
            buildTypes.getByName("debug").applicationIdSuffix = ".dev"
            // R8 would strip SDL's Java methods that native code calls through JNI
            buildTypes.getByName("release").proguardFile(rootProject.file("x2-sdl.pro"))
            // the game side, from the runtime's `x2app` target: libmain.so, libSDL3.so and SDL's
            // Java activity (java/), in <build>/app; X2_APP_NATIVE names that folder
            // (default: the repo's build-android/app)
            val repo = rootProject.projectDir.resolve("../../../..").canonicalFile
            val native = System.getenv("X2_APP_NATIVE") ?: repo.resolve("build-android/app").path
            sourceSets.getByName("main").java.srcDir(File(native, "java"))
            sourceSets.getByName("main").jniLibs.srcDir(rootProject.projectDir.resolve("build/x2-native"))
            val copyNative = tasks.register<Copy>("x2CopyNative") {
                from(native) { include("*.so") }
                into(rootProject.projectDir.resolve("build/x2-native/arm64-v8a"))
            }
            tasks.named("preBuild") { dependsOn(copyNative) }
            // release builds signed with the project's key when CI provides it (X2_KEYSTORE, a
            // file, and X2_KEYSTORE_PASSWORD): Android updates an app only with the same key
            System.getenv("X2_KEYSTORE")?.let { ks ->
                val key = signingConfigs.create("x2release") {
                    storeFile = File(ks)
                    storePassword = System.getenv("X2_KEYSTORE_PASSWORD")
                    keyAlias = System.getenv("X2_KEY_ALIAS") ?: "typex2"
                    keyPassword = System.getenv("X2_KEYSTORE_PASSWORD")
                }
                buildTypes.getByName("release").signingConfig = key
            }
        }
    }
}

// the runtime's native libraries are built for API 33 (Android 13, as Vulkan 1.3 needs); set
// when the DSL is final, after app/build.gradle.kts has set Tauri's default
subprojects {
    plugins.withId("com.android.application") {
        extensions.configure<com.android.build.api.variant.ApplicationAndroidComponentsExtension>("androidComponents") {
            finalizeDsl { it.defaultConfig.minSdk = 33 }
        }
    }
}
