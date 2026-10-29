package community.mcpp.mcppls

import com.google.gson.JsonObject
import com.google.gson.JsonParser
import com.intellij.execution.configurations.PathEnvironmentVariableUtil
import com.intellij.ide.plugins.PluginManagerCore
import com.intellij.openapi.extensions.PluginId
import com.intellij.openapi.util.SystemInfo
import com.intellij.util.system.CpuArch
import java.io.File

// Where the server is and the command that starts it (design 2026-10-29 §2.2). In order:
//
//   1. MCPPLS_SERVER, a program's absolute path (development and tests; the VS Code extension's name for it);
//   2. the payload this plugin carries, `<plugin>/payload`: the Marketplace's package for this system holds this
//      system's payload, and payload.json's `server` says what starts (the runner and the okb, or a program);
//   3. `mcppls` on PATH: the universal package, a single file or xlings, for a build of the plugin without a payload.
internal sealed interface Launch {
    data class Found(val command: List<String>, val source: String) : Launch
    // Something was named and cannot start; the reason says what to do.
    data class Refused(val reason: String) : Launch
    data object Missing : Launch
}

internal val PLUGIN_ID: PluginId = PluginId.getId("io.github.sunrisepeak.mcppls")

// The payload this installation carries; null when the plugin is not loaded from a directory.
internal fun carriedPayload(): File? = PluginManagerCore.getPlugin(PLUGIN_ID)?.pluginPath?.resolve("payload")?.toFile()

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

// The command payload.json's `server` names, with the executable bits a package may have lost put back: the runner,
// a program server, and the clangd the server starts.
internal fun payloadLaunch(payload: File, platform: String = currentPlatform()): Launch {
    val manifest = readManifest(payload)
        ?: return Launch.Refused("The plugin's payload manifest ${payload.resolve("payload.json")} cannot be read. Reinstall the plugin.")
    val built = manifest.get("platform")?.takeIf { it.isJsonPrimitive }?.asString
    if (built != null && built != platform) {
        return Launch.Refused("This package of the plugin is built for $built, and this machine is $platform. Install the plugin's package for this system.")
    }
    val command = serverCommand(payload, manifest)
        ?: return Launch.Refused("The language server in the plugin's payload ($payload) is missing or incomplete. Reinstall the plugin.")
    member(manifest, "clangd", "path")?.let { payload.resolve(it) }?.takeIf { it.isFile }?.let { runnable(it) }
    return Launch.Found(command, "payload")
}

// payload.json's `server`: the image through the runner it names (`bin/okrun bin/mcppls.okb`), or a program alone.
// Null when what it names is not there.
internal fun serverCommand(payload: File, manifest: JsonObject? = readManifest(payload)): List<String>? {
    val path = manifest?.let { member(it, "server", "path") } ?: return null
    val image = payload.resolve(path)
    if (!image.isFile) return null
    val runner = member(manifest, "server", "runner") ?: return listOf(runnable(image).path)
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
