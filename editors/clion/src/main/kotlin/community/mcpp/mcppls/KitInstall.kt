package community.mcpp.mcppls

import com.google.gson.JsonObject
import com.intellij.ide.util.PropertiesComponent
import com.intellij.notification.NotificationAction
import com.intellij.notification.NotificationGroupManager
import com.intellij.notification.NotificationType
import com.intellij.openapi.actionSystem.ActionUpdateThread
import com.intellij.openapi.actionSystem.AnActionEvent
import com.intellij.openapi.application.ApplicationManager
import com.intellij.openapi.ide.CopyPasteManager
import com.intellij.openapi.project.DumbAwareAction
import com.intellij.openapi.project.Project
import com.intellij.platform.lsp.api.Lsp4jClient
import com.intellij.platform.lsp.api.LspServerManager
import com.intellij.platform.lsp.api.LspServerNotificationsHandler
import java.awt.datatransfer.StringSelection
import org.eclipse.lsp4j.ExecuteCommandParams
import org.eclipse.lsp4j.jsonrpc.services.JsonNotification

// The semantic kit of a target (ecosystem design 2026-10-29 §9.2, S3-4-37 to S3-4-40). Without a compiler the server
// reads a project with the kit of its target; when that kit is not installed it reads with the openkal kit the
// payload carries -- `import std` and the standard library work, the system's own headers and macros do not -- and
// its status says so in a `kit-missing` issue, with `askInstall` when it can install the kit in the background
// (`mcppls.installKit`). This asks, as the VS Code extension does (editors/vscode/src/kitPrompt.ts):
//
//   - never blocking anything: a balloon, which may stay unanswered; the project is served meanwhile;
//   - once per project for one kit, and never again after "Don't Ask Again";
//   - "Always Install Kits" installs the next kits of this project without asking, and Tools | mcppls | Ask Before
//     Installing Semantic Kits takes it back;
//   - an answer to a question that no longer applies does nothing.
//
// How an install ended is told once, from the status's `kitInstall`.

internal const val KIT_MISSING = "kit-missing"
internal const val INSTALL_KIT_COMMAND = "mcppls.installKit"

// What the status says of a missing kit.
internal data class KitIssue(val target: String, val pkg: String, val version: String, val command: String, val message: String, val askInstall: Boolean) {
    // One question per kit: the package and its version.
    val key: String get() = "$pkg@$version"
}

internal fun kitIssue(status: JsonObject): KitIssue? {
    val issues = status.get("issues")?.takeIf { it.isJsonArray }?.asJsonArray ?: return null
    val issue = issues.mapNotNull { it.takeIf { it.isJsonObject }?.asJsonObject }
        .firstOrNull { it.get("code")?.takeIf { code -> code.isJsonPrimitive }?.asString == KIT_MISSING } ?: return null
    val kit = issue.get("kit")?.takeIf { it.isJsonObject }?.asJsonObject ?: return null
    fun text(from: JsonObject, key: String) = from.get(key)?.takeIf { it.isJsonPrimitive }?.asString ?: ""
    return KitIssue(
        target = text(kit, "target"),
        pkg = text(kit, "package"),
        version = text(kit, "version"),
        command = text(kit, "command"),
        message = text(issue, "message"),
        askInstall = issue.get("askInstall")?.takeIf { it.isJsonPrimitive }?.asBoolean == true,
    )
}

internal enum class KitAction { ASK, INSTALL, NONE }

// Install without asking on the person's say-so (once per kit, like the question), ask otherwise unless they said
// "Don't Ask Again", and nothing while a question is open or this kit was asked about already.
internal fun kitAction(issue: KitIssue?, never: Boolean, always: Boolean, askedAbout: Collection<String>, open: Boolean): KitAction = when {
    issue == null || !issue.askInstall || open || issue.key in askedAbout -> KitAction.NONE
    always -> KitAction.INSTALL
    never -> KitAction.NONE
    else -> KitAction.ASK
}

// How the last install the person asked for ended (S3-4-39), when it was not told yet.
internal data class KitInstall(val outcome: String, val message: String, val at: String)

internal fun kitInstallToTell(status: JsonObject, told: Collection<String>): KitInstall? {
    val run = status.get("kitInstall")?.takeIf { it.isJsonObject }?.asJsonObject ?: return null
    fun text(key: String) = run.get(key)?.takeIf { it.isJsonPrimitive }?.asString ?: ""
    val install = KitInstall(text("outcome"), text("message"), text("at"))
    if (install.at.isEmpty() || install.at in told || install.outcome !in setOf("installed", "failed")) return null
    return install
}

// The project's answers, in its workspace file (PropertiesComponent): never in the project's own files.
private object KitAnswers {
    const val NEVER = "community.mcpp.mcppls.kit.never"
    const val ALWAYS = "community.mcpp.mcppls.kit.always"
    const val ASKED = "community.mcpp.mcppls.kit.asked"
    const val TOLD = "community.mcpp.mcppls.kit.told"

    fun list(project: Project, key: String): List<String> = PropertiesComponent.getInstance(project).getList(key) ?: emptyList()

    fun remember(project: Project, key: String, value: String) {
        PropertiesComponent.getInstance(project).setList(key, (list(project, key) + value).takeLast(8))
    }
}

internal object KitPrompt {
    // Projects with a question open: the next status does not ask again.
    private val open = java.util.Collections.newSetFromMap(java.util.concurrent.ConcurrentHashMap<Project, Boolean>())
    // The kit each project still lacks, from its latest status.
    private val pending = java.util.concurrent.ConcurrentHashMap<Project, String>()

    fun onStatus(project: Project, status: JsonObject) {
        if (project.isDisposed) return
        tellInstall(project, status)
        val issue = kitIssue(status)
        if (issue != null) pending[project] = issue.target else pending.remove(project)
        val properties = PropertiesComponent.getInstance(project)
        val action = kitAction(issue, properties.getBoolean(KitAnswers.NEVER), properties.getBoolean(KitAnswers.ALWAYS),
            KitAnswers.list(project, KitAnswers.ASKED), project in open)
        if (action == KitAction.NONE || issue == null) return
        // Recorded before the answer: another status while the question is open must not ask again.
        KitAnswers.remember(project, KitAnswers.ASKED, issue.key)
        if (action == KitAction.INSTALL) {
            install(project, issue.target)
            return
        }
        open += project
        val notification = group().createNotification(
            "mcppls: the semantic kit for ${issue.target} is not installed",
            "Files are read with the openkal kit: import std and the standard library work, this system's own headers and " +
                "macros do not. Install ${issue.pkg} in the background (xlings reaches the network)?",
            NotificationType.INFORMATION,
        )
        notification.addAction(answer(project, "Install") { install(project, it) })
        notification.addAction(answer(project, "Always Install Kits") {
            PropertiesComponent.getInstance(project).setValue(KitAnswers.ALWAYS, true)
            install(project, it)
        })
        notification.addAction(NotificationAction.createSimpleExpiring("Copy Command") {
            open -= project
            CopyPasteManager.getInstance().setContents(StringSelection(issue.command))
        })
        notification.addAction(NotificationAction.createSimpleExpiring("Don't Ask Again") {
            open -= project
            PropertiesComponent.getInstance(project).setValue(KitAnswers.NEVER, true)
        })
        notification.whenExpired { open -= project }
        notification.notify(project)
    }

    // An answer acts on the kit the project still lacks; one installed meanwhile is left alone (S3-4-21).
    private fun answer(project: Project, text: String, act: (String) -> Unit) = NotificationAction.createSimpleExpiring(text) {
        open -= project
        pending[project]?.let(act)
    }

    fun askAgain(project: Project) {
        val properties = PropertiesComponent.getInstance(project)
        properties.unsetValue(KitAnswers.NEVER)
        properties.unsetValue(KitAnswers.ALWAYS)
        properties.unsetValue(KitAnswers.ASKED)
    }

    // `mcppls.installKit` to every mcppls server of the project, off the EDT.
    fun install(project: Project, target: String) {
        ApplicationManager.getApplication().executeOnPooledThread {
            if (project.isDisposed) return@executeOnPooledThread
            val arguments = listOf<Any>(JsonObject().apply { addProperty("target", target) })
            for (server in LspServerManager.getInstance(project).getServersForProvider(McpplsServerSupportProvider::class.java)) {
                runCatching { server.sendRequestSync { it.workspaceService.executeCommand(ExecuteCommandParams(INSTALL_KIT_COMMAND, arguments)) } }
            }
        }
    }

    private fun tellInstall(project: Project, status: JsonObject) {
        val run = kitInstallToTell(status, KitAnswers.list(project, KitAnswers.TOLD)) ?: return
        KitAnswers.remember(project, KitAnswers.TOLD, run.at)
        val installed = run.outcome == "installed"
        group().createNotification("mcppls", if (installed) "${run.message}; the project is read with it now." else run.message,
            if (installed) NotificationType.INFORMATION else NotificationType.WARNING).notify(project)
    }

    private fun group() = NotificationGroupManager.getInstance().getNotificationGroup("mcppls")
}

// The client side of mcppls's own notifications: `cxxModules/status` (S3 4), which the server sends to a client that
// declares `experimental.cxxModules.status` (McpplsServerDescriptor's capabilities).
internal class McpplsLspClient(handler: LspServerNotificationsHandler, private val project: Project) : Lsp4jClient(handler) {
    @JsonNotification("cxxModules/status")
    fun status(params: JsonObject) {
        KitPrompt.onStatus(project, params)
    }
}

// Tools | mcppls: takes back "Always Install Kits" and "Don't Ask Again" for this project.
internal class AskBeforeInstallingKitsAction : DumbAwareAction() {
    override fun actionPerformed(event: AnActionEvent) {
        event.project?.let { KitPrompt.askAgain(it) }
    }

    override fun getActionUpdateThread() = ActionUpdateThread.BGT
}
