package community.mcpp.mcppls

import com.intellij.execution.ExecutionException
import com.intellij.execution.configurations.GeneralCommandLine
import com.intellij.notification.NotificationGroupManager
import com.intellij.notification.NotificationType
import com.intellij.openapi.application.ApplicationManager
import com.intellij.openapi.application.ReadAction
import com.intellij.openapi.editor.event.EditorFactoryEvent
import com.intellij.openapi.editor.event.EditorFactoryListener
import com.intellij.openapi.fileEditor.FileDocumentManager
import com.intellij.openapi.project.Project
import com.intellij.openapi.util.Key
import com.intellij.openapi.vfs.VirtualFile
import com.intellij.platform.lsp.api.LspServerSupportProvider
import com.intellij.platform.lsp.api.ProjectWideLspServerDescriptor
import com.intellij.platform.lsp.api.customization.LspCompletionCustomizer
import com.intellij.platform.lsp.api.customization.LspCustomization

// Starts `mcppls serve` for C and C++ files. Everything else — finding the build, preparing the
// modules, driving clangd — happens in the server, which is why this file is this short.
//
// A file is answered by one engine: a project CLion models is CLion's and mcppls does not start for
// it, unless Settings | Tools | mcppls says otherwise (ClionModel). In the files mcppls answers, CLion's
// own engine answers too until the user lets mcppls answer them alone (OneEngine).
internal val CXX_EXTENSIONS = setOf(
    "cpp", "cxx", "cc", "c++", "c",
    "h", "hpp", "hxx", "hh",
    "cppm", "ccm", "cxxm", "c++m", "ixx", "mpp", "mxx",
    "mcpp",   // mcpp's build programs, build.mcpp
)

private val TOLD_NO_SERVER = Key.create<Boolean>("community.mcpp.mcppls.toldNoServer")

// Whether this project was told that no server can start (once per project); the tests read it.
internal fun toldNoServer(project: Project): Boolean = project.getUserData(TOLD_NO_SERVER) == true

internal class McpplsServerSupportProvider : LspServerSupportProvider {
    override fun fileOpened(
        project: Project,
        file: VirtualFile,
        serverStarter: LspServerSupportProvider.LspServerStarter,
    ) {
        if (!OneEngine.answers(project, file)) return
        when (val launch = resolveLaunch()) {
            is Launch.Found -> serverStarter.ensureServerStarted(McpplsServerDescriptor(project))
            is Launch.Refused -> tellOnce(project, launch.reason)
            Launch.Missing -> tellOnce(
                project,
                "This build of the plugin carries no language server for this system, and there is no mcppls on the PATH " +
                    "CLion sees. Install the plugin from the Marketplace (its package for this system carries the server), " +
                    "or put mcppls on the PATH: `xlings install mcppls`, or the release's universal package.",
            )
        }
    }

    private fun tellOnce(project: Project, reason: String) {
        if (project.getUserData(TOLD_NO_SERVER) == true) return
        project.putUserData(TOLD_NO_SERVER, true)
        NotificationGroupManager.getInstance().getNotificationGroup("mcppls")
            .createNotification("mcppls", reason, NotificationType.WARNING)
            .notify(project)
    }
}

// Every editor of a file, for OneEngine. The platform calls the provider's fileOpened to start a server, not for
// every file once one runs, so the files opened afterwards are seen here.
internal class McpplsEditorListener : EditorFactoryListener {
    override fun editorCreated(event: EditorFactoryEvent) {
        val project = event.editor.project ?: return
        val file = FileDocumentManager.getInstance().getFile(event.editor.document) ?: return
        if (project.isDisposed || !file.isInLocalFileSystem || file.extension?.lowercase() !in CXX_EXTENSIONS) return
        // Off the EDT: the decision reads the payload's manifest and CLion's workspace.
        ApplicationManager.getApplication().executeOnPooledThread {
            val answered = ReadAction.compute<Boolean, RuntimeException> { !project.isDisposed && OneEngine.answers(project, file) }
            if (answered && resolveLaunch() is Launch.Found) OneEngine.fileOpened(project, file)
        }
    }
}

private class McpplsServerDescriptor(project: Project) :
    ProjectWideLspServerDescriptor(project, "mcppls") {

    override fun isSupportedFile(file: VirtualFile) = file.extension?.lowercase() in CXX_EXTENSIONS

    // ServerLaunch says where: MCPPLS_SERVER, the payload this plugin carries, then PATH. fileOpened starts no
    // server unless one is found, so a refusal here is a payload that changed since.
    override fun createCommandLine(): GeneralCommandLine = when (val launch = resolveLaunch()) {
        is Launch.Found -> GeneralCommandLine(launch.command + "serve")
        is Launch.Refused -> throw ExecutionException(launch.reason)
        Launch.Missing -> throw ExecutionException("no mcppls to start")
    }

    // mcppls's completion items carry a mark, which "mcppls only" keeps (McpplsOnlyCompletion).
    override val lspCustomization: LspCustomization = object : LspCustomization() {
        override val completionCustomizer: LspCompletionCustomizer = McpplsCompletionSupport()
    }
}
