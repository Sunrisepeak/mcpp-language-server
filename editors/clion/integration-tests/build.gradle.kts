plugins {
    kotlin("jvm") version "2.4.0"
}

repositories {
    mavenCentral()
    maven("https://www.jetbrains.com/intellij-repository/releases")
    maven("https://packages.jetbrains.team/maven/p/ij/intellij-dependencies")
    maven("https://download.jetbrains.com/teamcity-repository")
    maven("https://cache-redirector.jetbrains.com/packages.jetbrains.team/maven/p/grazi/grazie-platform-public")
}

dependencies {
    testImplementation("com.jetbrains.intellij.tools:ide-starter-squashed:262.10968.63")
    testImplementation("com.jetbrains.intellij.tools:ide-starter-junit5:262.10968.63")
    testImplementation("com.jetbrains.intellij.tools:ide-starter-driver:262.10968.63")
    testImplementation("com.jetbrains.intellij.driver:driver-client:262.10968.63")
    testImplementation("com.jetbrains.intellij.driver:driver-sdk:262.10968.63")
    testImplementation("com.jetbrains.intellij.driver:driver-model:262.10968.63")
    testImplementation("org.junit.jupiter:junit-jupiter:5.12.2")
    testImplementation("org.kodein.di:kodein-di-jvm:7.20.2")
    testImplementation("org.jetbrains.kotlinx:kotlinx-coroutines-core-jvm:1.10.1")
    testRuntimeOnly("org.junit.platform:junit-platform-launcher")
}

kotlin { jvmToolchain(25) }

// This build has no dependency on the plugin's main output. Both IDE versions
// load the same packaged ZIP; compiling the plugin against each tested SDK
// would hide binary compatibility defects in the actual distributed plugin.
tasks.test {
    useJUnitPlatform()
    workingDir(layout.buildDirectory.dir("starter-work").get().asFile.also { it.mkdirs() })
    maxHeapSize = "2g"
    for (key in listOf("pluginZip", "ideVersion", "ideHome", "output", "expectedServerVersion")) {
        providers.gradleProperty(key).orNull?.let { systemProperty("mcppls.clion.$key", it) }
    }
    // The installed ZIP must supply its server and kit. No external server,
    // mcpp or compiler on PATH can make a broken carried payload pass.
    environment("PATH", "/usr/bin:/bin")
    environment("MCPPLS_SERVER", "")
    environment("SHELL", "/bin/sh")
    val isolatedHome = providers.gradleProperty("testHome").orNull?.let(::File)
        ?: layout.buildDirectory.dir("home").get().asFile
    isolatedHome.mkdirs()
    systemProperty("user.home", isolatedHome.absolutePath)
    environment("HOME", isolatedHome.absolutePath)
    environment("XDG_CONFIG_HOME", isolatedHome.resolve(".config").absolutePath)
    environment("XDG_DATA_HOME", isolatedHome.resolve(".local/share").absolutePath)
    environment("MCPP_HOME", isolatedHome.resolve(".mcpp").absolutePath)
    environment("MCPPLS_CACHE_DIR", isolatedHome.resolve("mcppls-cache").absolutePath)
    testLogging { events("passed", "failed", "skipped") }
}
