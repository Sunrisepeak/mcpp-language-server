package community.mcpp.mcppls

import com.google.gson.JsonParser
import com.intellij.execution.configurations.GeneralCommandLine
import com.intellij.execution.configurations.PathEnvironmentVariableUtil
import com.intellij.notification.NotificationGroupManager
import com.intellij.notification.NotificationType
import com.intellij.openapi.util.Key
import com.intellij.openapi.util.SystemInfo
import com.intellij.openapi.project.Project
import com.intellij.openapi.vfs.VirtualFile
import com.intellij.platform.lsp.api.LspServerSupportProvider
import com.intellij.platform.lsp.api.ProjectWideLspServerDescriptor
import java.io.File

// Starts `mcppls serve` for C and C++ files. Everything else — finding the build, preparing the
// modules, driving clangd — happens in the server, which is why this file is this short.
//
// A file is answered by one engine: a project CLion models is CLion's and mcppls does not start for
// it, unless Settings | Tools | mcppls says otherwise (ClionModel).
private val CXX_EXTENSIONS = setOf(
    "cpp", "cxx", "cc", "c++", "c",
    "h", "hpp", "hxx", "hh",
    "cppm", "ccm", "cxxm", "c++m", "ixx", "mpp", "mxx",
    "mcpp",   // mcpp's build programs, build.mcpp
)

private val TOLD_ABOUT_TWO_ENGINES = Key.create<Boolean>("community.mcpp.mcppls.toldAboutTwoEngines")

// Whether this project was told that both engines answer its files (once per project); the tests read it.
internal fun toldAboutTwoEngines(project: Project): Boolean = project.getUserData(TOLD_ABOUT_TWO_ENGINES) == true

internal class McpplsServerSupportProvider : LspServerSupportProvider {
    override fun fileOpened(
        project: Project,
        file: VirtualFile,
        serverStarter: LspServerSupportProvider.LspServerStarter,
    ) {
        if (file.extension?.lowercase() !in CXX_EXTENSIONS) return
        val modelled = ClionModel.models(project)
        if (modelled && !McpplsSettings.getInstance().alsoForModelledProjects) return
        serverStarter.ensureServerStarted(McpplsServerDescriptor(project))
        if (modelled) tellOnce(project)
    }

    // Once per project, the first time both engines answer a file.
    private fun tellOnce(project: Project) {
        if (project.getUserData(TOLD_ABOUT_TWO_ENGINES) == true) return
        project.putUserData(TOLD_ABOUT_TWO_ENGINES, true)
        NotificationGroupManager.getInstance().getNotificationGroup("mcppls")
            .createNotification(
                "mcppls",
                "CLion models this project, and mcppls is on for such projects (Settings | Tools | mcppls), " +
                    "so completion and diagnostics come from both engines.",
                NotificationType.INFORMATION,
            )
            .notify(project)
    }
}

private class McpplsServerDescriptor(project: Project) :
    ProjectWideLspServerDescriptor(project, "mcppls") {

    override fun isSupportedFile(file: VirtualFile) = file.extension?.lowercase() in CXX_EXTENSIONS

    // PATH first, the way every other tool the user installed is found: the universal package's
    // `mcppls`, a single file, or a build of one's own. Then the payload
    // `mcpp run -p devtools -- extension --editor clion --install` puts at <user data>/mcppls/payload
    // (tools/devtools/src/editors.cppm), started as its payload.json's `server` says.
    override fun createCommandLine(): GeneralCommandLine {
        PathEnvironmentVariableUtil.findExecutableInPathOnAnyOS("mcppls")?.let { return GeneralCommandLine(it.path, "serve") }
        return GeneralCommandLine((installedServer() ?: listOf("mcppls")) + "serve")
    }
}

// The command payload.json's `server` names in the installed payload: the platform's runner and the
// image (`server.runner`, `server.path`: `bin/okrun bin/mcppls.okb`), or a program alone. Null when
// there is no payload, or what it names is not there to start.
internal fun installedServer(payload: File? = installedPayload()): List<String>? {
    val manifest = payload?.resolve("payload.json")?.takeIf { it.isFile } ?: return null
    val server = runCatching { JsonParser.parseString(manifest.readText()).asJsonObject.getAsJsonObject("server") }.getOrNull()
        ?: return null
    val path = server.get("path")?.takeIf { it.isJsonPrimitive }?.asString ?: return null
    val runner = server.get("runner")?.takeIf { it.isJsonPrimitive }?.asString
    val image = payload.resolve(path)
    if (runner == null) return image.takeIf { it.canExecute() }?.let { listOf(it.path) }
    val program = payload.resolve(runner)
    return if (program.canExecute() && image.isFile) listOf(program.path, image.path) else null
}

// <user data>/mcppls/payload: $XDG_DATA_HOME or ~/.local/share on Linux, ~/Library/Application
// Support on macOS, %LOCALAPPDATA% on Windows.
private fun installedPayload(): File? {
    val home = System.getProperty("user.home") ?: return null
    return when {
        SystemInfo.isWindows -> System.getenv("LOCALAPPDATA")?.takeIf { it.isNotEmpty() }?.let { File(it, "mcppls\\payload") }
        SystemInfo.isMac -> File(home, "Library/Application Support/mcppls/payload")
        else -> {
            val data = System.getenv("XDG_DATA_HOME")?.takeIf { it.startsWith("/") } ?: "$home/.local/share"
            File(data, "mcppls/payload")
        }
    }
}
