package community.mcpp.mcppls

import com.intellij.openapi.actionSystem.ActionUpdateThread
import com.intellij.openapi.actionSystem.AnActionEvent
import com.intellij.openapi.project.DumbAwareAction

// Tools | mcppls: the two answers of OneEngine's question, at any time, for the current project.
internal class UseMcpplsOnlyAction : DumbAwareAction() {
    override fun actionPerformed(event: AnActionEvent) {
        event.project?.let { OneEngine.choose(it, Engines.MCPPLS_ONLY) }
    }

    override fun update(event: AnActionEvent) {
        event.presentation.isEnabled = event.project?.let { McpplsProjectSettings.of(it).effective != Engines.MCPPLS_ONLY } == true
    }

    override fun getActionUpdateThread() = ActionUpdateThread.BGT
}

internal class RestoreClionEngineAction : DumbAwareAction() {
    override fun actionPerformed(event: AnActionEvent) {
        event.project?.let { OneEngine.choose(it, Engines.BOTH) }
    }

    override fun update(event: AnActionEvent) {
        event.presentation.isEnabled = event.project?.let { McpplsProjectSettings.of(it).effective == Engines.MCPPLS_ONLY } == true
    }

    override fun getActionUpdateThread() = ActionUpdateThread.BGT
}

// Tools | mcppls: the server's own actions, under the ids `mcppls status` names for JetBrains IDEs (S3 section 8),
// so that mcpp's plugin reaches them through the ActionManager without holding ids of its own.
internal class RestartServerAction : DumbAwareAction() {
    override fun actionPerformed(event: AnActionEvent) {
        val project = event.project ?: return
        com.intellij.platform.lsp.api.LspServerManager.getInstance(project)
            .stopAndRestartIfNeeded(McpplsServerSupportProvider::class.java)
    }

    override fun update(event: AnActionEvent) {
        event.presentation.isEnabled = event.project != null
    }

    override fun getActionUpdateThread() = ActionUpdateThread.BGT
}

internal class ResetWorkspaceCacheAction : DumbAwareAction() {
    override fun actionPerformed(event: AnActionEvent) {
        val project = event.project ?: return
        val servers = com.intellij.platform.lsp.api.LspServerManager.getInstance(project)
            .getServersForProvider(McpplsServerSupportProvider::class.java)
        com.intellij.openapi.application.ApplicationManager.getApplication().executeOnPooledThread {
            // S3 5.6: the workspace's cache is removed and the root is read again.
            for (server in servers) {
                server.sendRequestSync { languageServer ->
                    languageServer.workspaceService.executeCommand(
                        org.eclipse.lsp4j.ExecuteCommandParams("mcppls.resetCache", emptyList()))
                }
            }
        }
    }

    override fun update(event: AnActionEvent) {
        event.presentation.isEnabled = event.project?.let {
            com.intellij.platform.lsp.api.LspServerManager.getInstance(it)
                .getServersForProvider(McpplsServerSupportProvider::class.java).isNotEmpty()
        } == true
    }

    override fun getActionUpdateThread() = ActionUpdateThread.BGT
}
