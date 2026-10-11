// The CLion plugin: registers mcppls as a language server for C and C++ through the IntelliJ
// platform's LSP API. That API is available in the paid IDEs, which CLion is.
//
// Build it with `mcpp run --features clion mcppls-devtools -- extension --editor clion`, which runs
// `gradle buildPlugin` here. Gradle and the JDK it runs on come from `xim:gradle`; the JDK Kotlin
// compiles with is the toolchain below, resolved by the plugin in settings.gradle.kts.
import org.jetbrains.intellij.platform.gradle.IntelliJPlatformType
import org.jetbrains.intellij.platform.gradle.TestFrameworkType
import org.jetbrains.kotlin.gradle.dsl.JvmDefaultMode

plugins {
    id("java")
    id("org.jetbrains.kotlin.jvm") version "2.4.0"
    id("org.jetbrains.intellij.platform") version "2.19.0"
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
        // The C/C++ plugin's workspace classes, which CidrWorkspaceProbe asks whether CLion models the project.
        bundledPlugin("com.intellij.clion")
        // The platform's own test framework: a real CLion (src/test/kotlin), headless.
        testFramework(TestFrameworkType.Platform)
    }
    testImplementation("junit:junit:4.13.2")
    testImplementation("org.opentest4j:opentest4j:1.3.0")
}

kotlin {
    jvmToolchain(21)
    // Built against 2026.2.3 but run on 2025.2 too. Without this Kotlin copies the platform
    // interfaces' default methods into our classes as calls to `super`, and 2026.2's renamed LSP
    // interface has defaults 2025.2 does not (the verifier's NoSuchMethodError).
    compilerOptions { jvmDefault = JvmDefaultMode.NO_COMPATIBILITY }
}

intellijPlatform {
    pluginConfiguration {
        ideaVersion {
            sinceBuild = "252"
            untilBuild = provider { null }
        }
    }
    // `gradle verifyPlugin`: the plugin against the oldest CLion it claims (sinceBuild 252) and the
    // one it is built on. Each download is about 1.9 GB; CI caches them by version.
    pluginVerification {
        ides {
            create(IntelliJPlatformType.CLion, "2025.2")
            create(IntelliJPlatformType.CLion, providers.gradleProperty("platformVersion").get())
        }
    }
}

// The tests start the server the way CLion does, from the PATH it sees. CI puts the payload it built
// first (MCPPLS_PAYLOAD_BIN); a developer points it at any payload's bin directory. `mcpp` is left off
// the PATH on purpose: the test's project is an mcpp package, and with no mcpp to ask the server reads
// its sources, which is what a test of the plugin needs and what finishes in seconds.
tasks.test {
    val bin = providers.environmentVariable("MCPPLS_PAYLOAD_BIN").orNull
    if (bin != null) environment("PATH", "$bin:/usr/bin:/bin")
    environment("MCPPLS_CACHE_DIR", layout.buildDirectory.dir("mcppls-cache").get().asFile.absolutePath)
    // The server reads the login shell's environment to find build tools; an empty home and a plain
    // `sh` keep whatever the machine's profile adds (an installed mcpp, say) out of the test.
    environment("HOME", layout.buildDirectory.dir("home").get().asFile.also { it.mkdirs() }.absolutePath)
    environment("SHELL", "/bin/sh")
    systemProperty("mcppls.fixtures", layout.projectDirectory.dir("src/test/fixtures").asFile.absolutePath)
    // CLion rejects its backend freeze watchdog in unit tests.
    // Server startup and shutdown deadlines remain independently bounded.
    systemProperty("patch.engine.backend.freeze.timeout", "0")
    // Every wait in the tests is bounded; a test that never finishes is the one to report.
    testLogging { showStandardStreams = true; events("passed", "failed", "skipped") }
}
