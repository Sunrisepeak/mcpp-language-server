// The Kotlin compiler needs the JDK `build.gradle.kts` asks for, not whichever one Gradle happens
// to be running on. `xim:gradle` brings Temurin 25; Kotlin 2.1 compiling on it fails with an
// internal compiler error 11 minutes into the build, after the CLion SDK has been downloaded.
//
// Gradle cannot fetch a toolchain without a resolver, so declaring `jvmToolchain(21)` on its own
// silently falls back to the running JVM — which is how that happens. This makes the declaration
// mean what it says.
plugins {
    id("org.gradle.toolchains.foojay-resolver-convention") version "0.9.0"
}

rootProject.name = "mcppls-clion"
