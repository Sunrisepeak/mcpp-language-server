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
            for (server in servers) executeCommand(server, "mcppls.resetCache")
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

/**
 * Sends `workspace/executeCommand` to a server. `sendRequestSync` is LspServer's in 2026.1 and its
 * supertype LspClient's from 2026.2, and Kotlin names the declaring type in the call, which 2026.1
 * does not have; the method is therefore found on the server's class, where both lines have it.
 */
internal fun executeCommand(server: com.intellij.platform.lsp.api.LspServer, command: String) {
    val send = server.javaClass.methods.firstOrNull {
        it.name == "sendRequestSync" && it.parameterTypes.lastOrNull() == kotlin.jvm.functions.Function1::class.java
    } ?: return com.intellij.openapi.diagnostic.Logger.getInstance("mcppls").warn("this IDE's LSP client has no sendRequestSync; $command was not sent")
    val handler: (org.eclipse.lsp4j.services.LanguageServer) -> java.util.concurrent.CompletableFuture<Any> = { languageServer ->
        languageServer.workspaceService.executeCommand(org.eclipse.lsp4j.ExecuteCommandParams(command, emptyList()))
    }
    val arguments: Array<Any?> = if (send.parameterCount == 2) arrayOf(30_000, handler) else arrayOf(handler)
    runCatching { send.invoke(server, *arguments) }
        .onFailure { com.intellij.openapi.diagnostic.Logger.getInstance("mcppls").warn("$command was not sent", it) }
}
