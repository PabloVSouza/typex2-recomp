# SDL's Java side is called from native code through JNI, which R8 cannot see: keep all of it
# (added to release builds by build.gradle.kts).
-keep class org.libsdl.app.** { *; }
