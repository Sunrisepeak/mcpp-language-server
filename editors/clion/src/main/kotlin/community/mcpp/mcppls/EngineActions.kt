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
