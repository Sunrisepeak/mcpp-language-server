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
    // Built against 2026.2.3 but run on 2026.1 too. Without this Kotlin copies the platform
    // interfaces' default methods into our classes as calls to `super`, and 2026.2's renamed LSP
    // interface has defaults an older platform does not (the verifier's NoSuchMethodError).
    compilerOptions { jvmDefault = JvmDefaultMode.NO_COMPATIBILITY }
}

// The payload each package carries (design 2026-10-29 §2.1): `-Pmcppls.payload.<platform>=<dir>` for the
// platforms of packaging/release.manifest.json, each a payload directory as a release's
// payload-<platform>.tar.gz unpacks it (named `payload`, holding payload.json). MCPPLS_PAYLOAD names this
// machine's, for a developer and the tests. With any of them the build makes one package per system and
// architecture, each with its payload at `<plugin>/payload`, and the sandbox the tests run in is this
// machine's; with none, the plugin carries no server (a build for development).
val platforms = listOf("linux-x64", "linux-arm64", "darwin-arm64", "win32-x64")
val hostPlatform: String = run {
    val os = System.getProperty("os.name").lowercase()
    val system = when {
        os.startsWith("windows") -> "win32"
        os.startsWith("mac") -> "darwin"
        else -> "linux"
    }
    val arch = System.getProperty("os.arch").lowercase()
    "$system-${if (arch == "aarch64" || arch == "arm64") "arm64" else "x64"}"
}
val payloads: Map<String, File> = platforms.mapNotNull { platform ->
    val named = providers.gradleProperty("mcppls.payload.$platform").orNull
        ?: providers.environmentVariable("MCPPLS_PAYLOAD").orNull?.takeIf { platform == hostPlatform }
    named?.let { platform to File(it).absoluteFile }
}.toMap()
payloads.forEach { (platform, directory) ->
    require(directory.name == "payload" && directory.resolve("payload.json").isFile) {
        "the payload for $platform must be a directory named payload holding payload.json: $directory"
    }
}

intellijPlatform {
    pluginConfiguration {
        // 2026.1 is the first IDE the Marketplace gives a package for its own system and architecture.
        ideaVersion {
            sinceBuild = "261"
            untilBuild = provider { null }
        }
    }
    nativeVariants {
        enabled = payloads.isNotEmpty()
        // A directory named `payload` lands as `<plugin>/payload`: the tree keeps the paths below its root.
        fun carried(platform: String) = payloads[platform]?.let { directory ->
            fileTree(directory.parentFile) { include("${directory.name}/**") }
        } ?: files()
        linux.x86_64.from(carried("linux-x64"))
        linux.arm64.from(carried("linux-arm64"))
        mac.arm64.from(carried("darwin-arm64"))
        windows.x86_64.from(carried("win32-x64"))
    }
    // `gradle verifyPlugin`: the plugin against the oldest CLion it claims (sinceBuild 261) and the
    // one it is built on. Each download is about 1.9 GB; CI caches them by version.
    pluginVerification {
        ides {
            create(IntelliJPlatformType.CLion, "2026.1")
            create(IntelliJPlatformType.CLion, providers.gradleProperty("platformVersion").get())
        }
    }
}

// The tests start the server the way CLion does with nothing on the PATH: the payload the plugin
// carries, which the sandbox of this machine's package holds when MCPPLS_PAYLOAD names one (CI names
// the payload it built), started as its payload.json's `server` says. `mcpp` is left off the PATH on
// purpose: the test's project is an mcpp package, and with no mcpp to ask the server reads its
// sources, which is what a test of the plugin needs and what finishes in seconds.
tasks.test {
    val home = layout.buildDirectory.dir("home").get().asFile.also { it.mkdirs() }
    environment("PATH", "/usr/bin:/bin")
    environment("XDG_DATA_HOME", home.resolve(".local/share").absolutePath)
    systemProperty("mcppls.carriesPayload", payloads.containsKey(hostPlatform).toString())
    environment("MCPPLS_CACHE_DIR", layout.buildDirectory.dir("mcppls-cache").get().asFile.absolutePath)
    // The server reads the login shell's environment to find build tools; an empty home and a plain
    // `sh` keep whatever the machine's profile adds (an installed mcpp, say) out of the test.
    environment("HOME", home.absolutePath)
    environment("SHELL", "/bin/sh")
    systemProperty("mcppls.fixtures", layout.projectDirectory.dir("src/test/fixtures").asFile.absolutePath)
    // MCPPLS_FIXTURES_BUILT=1: the `mcpp-built` fixture was built by mcpp before the suite (CI does it).
    systemProperty("mcppls.fixturesBuilt", (providers.environmentVariable("MCPPLS_FIXTURES_BUILT").orNull == "1").toString())
    // CLion rejects its backend freeze watchdog in unit tests.
    // Server startup and shutdown deadlines remain independently bounded.
    systemProperty("patch.engine.backend.freeze.timeout", "0")
    // Every wait in the tests is bounded; a test that never finishes is the one to report.
    testLogging { showStandardStreams = true; events("passed", "failed", "skipped") }
}

// A program of the payload (its runner, clangd) keeps its executable bit in this system's package, as it has
// in the payload; the plugin also puts the bit back when an installation loses it (ServerLaunch.kt). The bits
// are read from the payloads themselves: the archive's own source is a copy that lost them.
val programs: Set<String> = payloads.values.flatMap { directory ->
    directory.walkTopDown().filter { it.isFile && it.canExecute() }
        .map { "${directory.name}/${it.relativeTo(directory).invariantSeparatorsPath}" }.toList()
}.toSet()
tasks.withType<Zip>().matching { it.name.startsWith("buildPluginVariants_") }.configureEach {
    eachFile {
        val path = relativePath.pathString
        if (programs.any { path == it || path.endsWith("/$it") }) permissions { unix("rwxr-xr-x") }
    }
}
