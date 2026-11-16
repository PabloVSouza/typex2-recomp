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
            // the game side: SDL's activity (Java, from the SDL checkout) and the native
            // libraries built by the runtime's `x2app` target (libmain.so, libSDL3.so);
            // X2_APP_NATIVE overrides where they are (default: the repo's build-android/app)
            val repo = rootProject.projectDir.resolve("../../../..").canonicalFile
            val native = System.getenv("X2_APP_NATIVE") ?: repo.resolve("build-android/app").path
            sourceSets.getByName("main").java.srcDir(repo.resolve("third_party/SDL/android-project/app/src/main/java"))
            sourceSets.getByName("main").jniLibs.srcDir(rootProject.projectDir.resolve("build/x2-native"))
            val copyNative = tasks.register<Copy>("x2CopyNative") {
                from(native) { include("*.so") }
                into(rootProject.projectDir.resolve("build/x2-native/arm64-v8a"))
            }
            tasks.named("preBuild") { dependsOn(copyNative) }
        }
    }
}
