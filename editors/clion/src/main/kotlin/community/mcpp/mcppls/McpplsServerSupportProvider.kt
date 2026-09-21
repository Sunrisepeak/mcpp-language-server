package community.mcpp.mcppls

import com.intellij.execution.configurations.GeneralCommandLine
import com.intellij.execution.configurations.PathEnvironmentVariableUtil
import com.intellij.openapi.util.SystemInfo
import com.intellij.openapi.project.Project
import com.intellij.openapi.vfs.VirtualFile
import com.intellij.platform.lsp.api.LspServerSupportProvider
import com.intellij.platform.lsp.api.ProjectWideLspServerDescriptor
import java.io.File

// Starts `mcppls serve` for C and C++ files. Everything else — finding the build, preparing the
// modules, driving clangd — happens in the server, which is why this file is this short.
private val CXX_EXTENSIONS = setOf(
    "cpp", "cxx", "cc", "c++", "c",
    "h", "hpp", "hxx", "hh",
    "cppm", "ccm", "cxxm", "c++m", "ixx", "mpp", "mxx",
)

internal class McpplsServerSupportProvider : LspServerSupportProvider {
    override fun fileOpened(
        project: Project,
        file: VirtualFile,
        serverStarter: LspServerSupportProvider.LspServerStarter,
    ) {
        if (file.extension?.lowercase() in CXX_EXTENSIONS) {
            serverStarter.ensureServerStarted(McpplsServerDescriptor(project))
        }
    }
}

private class McpplsServerDescriptor(project: Project) :
    ProjectWideLspServerDescriptor(project, "mcppls") {

    override fun isSupportedFile(file: VirtualFile) = file.extension?.lowercase() in CXX_EXTENSIONS

    // PATH first, the way every other tool the user installed is found; then the server
    // `mcpp run -p devtools -- extension --editor clion --install` puts at
    // <user data>/mcppls/payload (tools/devtools/src/editors.cppm).
    override fun createCommandLine(): GeneralCommandLine {
        val onPath = PathEnvironmentVariableUtil.findExecutableInPathOnAnyOS("mcppls")
        val server = onPath?.path ?: installedServer()?.takeIf { File(it).canExecute() } ?: "mcppls"
        return GeneralCommandLine(server, "serve")
    }
}

// <user data>/mcppls/payload/bin/mcppls: $XDG_DATA_HOME or ~/.local/share on Linux,
// ~/Library/Application Support on macOS, %LOCALAPPDATA% on Windows.
private fun installedServer(): String? {
    val home = System.getProperty("user.home") ?: return null
    return when {
        SystemInfo.isWindows -> System.getenv("LOCALAPPDATA")?.takeIf { it.isNotEmpty() }
            ?.let { "$it\\mcppls\\payload\\bin\\mcppls.exe" }
        SystemInfo.isMac -> "$home/Library/Application Support/mcppls/payload/bin/mcppls"
        else -> {
            val data = System.getenv("XDG_DATA_HOME")?.takeIf { it.startsWith("/") } ?: "$home/.local/share"
            "$data/mcppls/payload/bin/mcppls"
        }
    }
}
