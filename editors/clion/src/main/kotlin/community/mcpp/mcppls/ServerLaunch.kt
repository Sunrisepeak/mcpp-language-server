package community.mcpp.mcppls

import com.google.gson.JsonObject
import com.google.gson.JsonParser
import com.intellij.execution.configurations.PathEnvironmentVariableUtil
import com.intellij.openapi.application.PathManager
import com.intellij.openapi.util.SystemInfo
import com.intellij.util.system.CpuArch
import java.io.File

// Where the server is and the command that starts it (design 2026-10-29 §2.2; ecosystem design 2026-10-29 §9). In order:
//
//   1. MCPPLS_SERVER, a program's absolute path (development and tests; the VS Code extension's name for it);
//   2. the payload this plugin carries, `<plugin>/payload`: one payload for every system (payload version 5), whose
//      entry for this platform names the runner that starts the server image and the engine it drives (a payload of
//      version 4 and earlier is one platform's: its `server` says what starts, the runner and the okb, or a program);
//   3. `mcppls` on PATH: the openkal tree, a single file or xlings, for a build of the plugin without a payload.
internal sealed interface Launch {
    data class Found(val command: List<String>, val source: String) : Launch
    // Something was named and cannot start; the reason says what to do.
    data class Refused(val reason: String) : Launch
    data object Missing : Launch
}

// The payload this installation carries, `<plugin>/payload` (looking a plugin up by its id is the platform's internal
// API): the plugin directory this class's jar is in (`<plugin>/lib/<jar>`), else the one the IDE installs plugins as
// (`<plugins>/mcppls-clion`, the archive's top directory), which is also the tests' sandbox. Null when neither holds one.
internal fun carriedPayload(): File? {
    val fromJar = runCatching {
        File(Launch::class.java.protectionDomain.codeSource.location.toURI()).parentFile?.parentFile?.resolve("payload")
    }.getOrNull()
    val installed = runCatching { PathManager.getPluginsDir().resolve("mcppls-clion/payload").toFile() }.getOrNull()
    return listOfNotNull(fromJar, installed).firstOrNull { it.resolve("payload.json").isFile }
}

// The payload platforms (packaging/release.manifest.json): `${process.platform}-${process.arch}` as Node names them.
internal fun currentPlatform(): String {
    val system = when {
        SystemInfo.isWindows -> "win32"
        SystemInfo.isMac -> "darwin"
        else -> "linux"
    }
    return "$system-${if (CpuArch.isArm64()) "arm64" else "x64"}"
}

internal fun resolveLaunch(
    environment: Map<String, String> = System.getenv(),
    payload: File? = carriedPayload(),
    platform: String = currentPlatform(),
    onPath: () -> File? = { PathEnvironmentVariableUtil.findExecutableInPathOnAnyOS("mcppls") },
): Launch {
    environment["MCPPLS_SERVER"]?.takeIf { it.isNotEmpty() }?.let { named ->
        val program = File(named)
        if (!program.isAbsolute || !program.isFile) return Launch.Refused("MCPPLS_SERVER does not name an existing program: $named")
        return Launch.Found(listOf(runnable(program).path), "MCPPLS_SERVER")
    }
    if (payload != null && payload.resolve("payload.json").isFile) return payloadLaunch(payload, platform)
    onPath()?.let { return Launch.Found(listOf(it.path), "PATH") }
    return Launch.Missing
}

// The command payload.json names for `platform`, with the executable bits a package may have lost put back: the runner,
// a program server, and the clangd the server starts. A payload with no entry for `platform` is refused, saying which
// platforms it carries.
internal fun payloadLaunch(payload: File, platform: String = currentPlatform()): Launch {
    val manifest = readManifest(payload)
        ?: return Launch.Refused("The plugin's payload manifest ${payload.resolve("payload.json")} cannot be read. Reinstall the plugin.")
    if (platformPart(manifest, platform) == null) {
        val carried = payloadPlatforms(manifest).joinToString(", ").ifEmpty { "none" }
        return Launch.Refused("The plugin's payload carries no server for $platform; it carries $carried. Reinstall the plugin, or put mcppls on the PATH.")
    }
    val command = serverCommand(payload, manifest, platform)
        ?: return Launch.Refused("The language server in the plugin's payload ($payload) is missing or incomplete. Reinstall the plugin.")
    platformPart(manifest, platform)?.clangd?.let { payload.resolve(it) }?.takeIf { it.isFile }?.let { runnable(it) }
    return Launch.Found(command, "payload")
}

// This platform's part of a payload: the runner of the server image (none for a program server) and the engine.
internal data class PlatformPart(val runner: String?, val clangd: String?)

// Payload version 5 has an entry per platform; versions 1 to 4 are the one platform they name (a payload naming none
// was assembled for the machine it is on).
internal fun platformPart(manifest: JsonObject, platform: String): PlatformPart? {
    manifest.get("platforms")?.takeIf { it.isJsonObject }?.asJsonObject?.let { platforms ->
        val entry = platforms.get(platform)?.takeIf { it.isJsonObject }?.asJsonObject ?: return null
        val runner = entry.get("runner")?.takeIf { it.isJsonPrimitive }?.asString
        val clangd = entry.get("engines")?.takeIf { it.isJsonObject }?.asJsonObject?.let { member(it, "clangd", "path") }
        return PlatformPart(runner, clangd)
    }
    val built = manifest.get("platform")?.takeIf { it.isJsonPrimitive }?.asString
    if (built != null && built != platform) return null
    return PlatformPart(member(manifest, "server", "runner"), member(manifest, "clangd", "path"))
}

internal fun payloadPlatforms(manifest: JsonObject): List<String> =
    manifest.get("platforms")?.takeIf { it.isJsonObject }?.asJsonObject?.keySet()?.toList()
        ?: listOfNotNull(manifest.get("platform")?.takeIf { it.isJsonPrimitive }?.asString)

// What starts the server on `platform`: the image through this platform's runner (`libexec/okrun/<system>/okrun
// bin/mcppls.okb`), or a program alone. Null when what it names is not there.
internal fun serverCommand(payload: File, manifest: JsonObject? = readManifest(payload), platform: String = currentPlatform()): List<String>? {
    val path = manifest?.let { member(it, "server", "path") } ?: return null
    val image = payload.resolve(path)
    if (!image.isFile) return null
    val part = platformPart(manifest, platform) ?: return null
    val runner = part.runner ?: return listOf(runnable(image).path)
    val program = payload.resolve(runner)
    return if (program.isFile) listOf(runnable(program).path, image.path) else null
}

private fun readManifest(payload: File): JsonObject? = runCatching {
    JsonParser.parseString(payload.resolve("payload.json").readText()).asJsonObject
}.getOrNull()

private fun member(manifest: JsonObject, section: String, key: String): String? =
    manifest.get(section)?.takeIf { it.isJsonObject }?.asJsonObject?.get(key)?.takeIf { it.isJsonPrimitive }?.asString

// A package may not keep the executable bit (the Marketplace's and a disk install's unpacking); put it back when the
// file allows. A read-only installation keeps its modes, and starting it reports the problem.
private fun runnable(file: File): File {
    if (!SystemInfo.isWindows && !file.canExecute()) file.setExecutable(true)
    return file
}
