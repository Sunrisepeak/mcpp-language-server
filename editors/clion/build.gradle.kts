// The CLion plugin: registers mcppls as a language server for C and C++ through the IntelliJ
// platform's LSP API. That API is available in the paid IDEs, which CLion is.
//
// Build it with `mcpp run --features clion mcppls-devtools -- extension --editor clion`, which runs
// `gradle buildPlugin` here. Gradle and the JDK it runs on come from `xim:gradle`; the JDK Kotlin
// compiles with is the toolchain below, resolved by the plugin in settings.gradle.kts.
plugins {
    id("java")
    id("org.jetbrains.kotlin.jvm") version "2.2.20"
    id("org.jetbrains.intellij.platform") version "2.2.1"
}

group = "io.github.sunrisepeak"
version = providers.gradleProperty("pluginVersion").get()

repositories {
    mavenCentral()
    intellijPlatform { defaultRepositories() }
}

dependencies {
    intellijPlatform {
        create(providers.gradleProperty("platformType").get(), providers.gradleProperty("platformVersion").get())
    }
}

kotlin { jvmToolchain(21) }

intellijPlatform {
    pluginConfiguration {
        ideaVersion {
            sinceBuild = "252"
            untilBuild = provider { null }
        }
    }
}
