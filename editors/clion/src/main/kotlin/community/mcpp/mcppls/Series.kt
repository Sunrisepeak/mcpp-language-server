package community.mcpp.mcppls

import java.io.File
import java.util.Properties
import java.util.concurrent.TimeUnit

// The series rule (docs/92-release.md "Versions"): a version is X.Y.Z, X.Y the series the server and every plugin share,
// Z each component's own patch. The plugin starts a server of its series and refuses one of another, naming both
// versions and the install that matches.

private val threePart = Regex("""^(\d+)\.(\d+)\.(\d+)(?:[-+].*)?$""")

// The X.Y of a three-part version; null for any other text.
internal fun seriesOf(version: String): String? = threePart.matchEntire(version.trim())?.let { "${it.groupValues[1]}.${it.groupValues[2]}" }

// Why the plugin cannot start this server, or null when it can. A server of unknown version is not refused.
internal fun seriesMismatch(pluginVersion: String, serverVersion: String?, where: String): String? {
    if (serverVersion == null) return null
    val ours = seriesOf(pluginVersion) ?: return null
    val theirs = seriesOf(serverVersion) ?: return null
    if (ours == theirs) return null
    return "The language server $where is mcppls $serverVersion (series $theirs); this plugin, mcppls for CLion $pluginVersion, " +
        "works with series $ours. Reinstall the plugin to restore the server it carries, or install a server of series " +
        "$ours (xlings install mcppls@$ours)."
}

// This plugin's version, which the build writes into mcppls-clion.properties (build.gradle.kts).
internal fun pluginVersion(): String = runCatching {
    Properties().apply {
        Launch::class.java.getResourceAsStream("/mcppls-clion.properties")?.use { load(it) }
    }.getProperty("version")
}.getOrNull().orEmpty()

// The version `mcppls --version` prints, or null when it does not answer within a few seconds.
internal fun reportedVersion(program: File): String? = runCatching {
    val process = ProcessBuilder(program.path, "--version").redirectErrorStream(true).start()
    process.outputStream.close()
    if (!process.waitFor(10, TimeUnit.SECONDS)) {
        process.destroyForcibly()
        return null
    }
    Regex("""\b\d+\.\d+\.\d+\b""").find(process.inputStream.bufferedReader().readText())?.value
}.getOrNull()
