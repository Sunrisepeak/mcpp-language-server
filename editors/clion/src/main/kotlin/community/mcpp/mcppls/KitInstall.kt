package community.mcpp.mcppls

import com.google.gson.JsonObject
import com.intellij.execution.configurations.GeneralCommandLine
import com.intellij.execution.configurations.PathEnvironmentVariableUtil
import com.intellij.execution.util.ExecUtil
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
import com.intellij.openapi.util.SystemInfo
import com.intellij.platform.lsp.api.Lsp4jClient
import com.intellij.platform.lsp.api.LspServerNotificationsHandler
import java.awt.datatransfer.StringSelection
import java.io.File
import org.eclipse.lsp4j.jsonrpc.services.JsonNotification

// The semantic kit of a target (ecosystem design 2026-10-29 §9.2, S3-4-37 to S3-4-40). Without a compiler the server
// reads a project with the kit of its target; when that kit is not installed it reads with the openkal kit the
// payload carries -- `import std` and the standard library work, the system's own headers and macros do not -- and
// its status says so in a `kit-missing` issue, with `askInstall` when xlings is there to install it. This asks, as the
// VS Code extension does (editors/vscode/src/kitPrompt.ts), and runs the issue's own command (`xlings install
// mcppls-kit-<target>@<version>`) in the background; the server finds the installed kit by itself within half a
// minute and reads the project with it (the LSP API's requests are not the same in every CLion this supports):
//
//   - never blocking anything: a balloon, which may stay unanswered; the project is served meanwhile;
//   - once per project for one kit, and never again after "Don't Ask Again";
//   - "Always Install Kits" installs the next kits of this project without asking, and Tools | mcppls | Ask Before
//     Installing Semantic Kits takes it back;
//   - an answer to a question that no longer applies does nothing.
//
// How an install ended is told once.

internal const val KIT_MISSING = "kit-missing"

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
    private val pending = java.util.concurrent.ConcurrentHashMap<Project, KitIssue>()
    // Projects with an install running: one at a time.
    private val installing = java.util.Collections.newSetFromMap(java.util.concurrent.ConcurrentHashMap<Project, Boolean>())

    fun onStatus(project: Project, status: JsonObject) {
        if (project.isDisposed) return
        tellInstall(project, status)
        val issue = kitIssue(status)
        if (issue != null) pending[project] = issue else pending.remove(project)
        val properties = PropertiesComponent.getInstance(project)
        val action = kitAction(issue, properties.getBoolean(KitAnswers.NEVER), properties.getBoolean(KitAnswers.ALWAYS),
            KitAnswers.list(project, KitAnswers.ASKED), project in open)
        if (action == KitAction.NONE || issue == null) return
        // Recorded before the answer: another status while the question is open must not ask again.
        KitAnswers.remember(project, KitAnswers.ASKED, issue.key)
        if (action == KitAction.INSTALL) {
            install(project, issue)
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
    private fun answer(project: Project, text: String, act: (KitIssue) -> Unit) = NotificationAction.createSimpleExpiring(text) {
        open -= project
        pending[project]?.let(act)
    }

    fun askAgain(project: Project) {
        val properties = PropertiesComponent.getInstance(project)
        properties.unsetValue(KitAnswers.NEVER)
        properties.unsetValue(KitAnswers.ALWAYS)
        properties.unsetValue(KitAnswers.ASKED)
    }

    // The issue's own command, `xlings install mcppls-kit-<target>@<version>`, run off the EDT; its end is told once.
    fun install(project: Project, issue: KitIssue) {
        if (!installing.add(project)) return
        ApplicationManager.getApplication().executeOnPooledThread {
            try {
                val words = issue.command.split(' ').filter { it.isNotEmpty() }
                val xlings = findXlings()
                if (xlings == null || words.size < 3 || words[0] != "xlings") {
                    tell(project, false, "${issue.pkg} cannot be installed here: xlings is not installed. Install xlings, or unpack the release's " +
                        "${issue.pkg}-<version>.tar.gz into its store.")
                    return@executeOnPooledThread
                }
                val line = GeneralCommandLine(listOf(xlings.path) + words.drop(1) + "-y")
                val output = ExecUtil.execAndGetOutput(line, 15 * 60 * 1000)
                if (output.exitCode == 0 && !output.isTimeout) {
                    tell(project, true, "${issue.pkg} is installed; the project is read with it within half a minute.")
                } else {
                    val why = (output.stderr.ifBlank { output.stdout }).lines().takeLast(6).joinToString("\n")
                    tell(project, false, "${issue.command} failed${if (output.isTimeout) " (timed out)" else ""}: $why")
                }
            } finally {
                installing.remove(project)
            }
        }
    }

    private fun findXlings(): File? {
        PathEnvironmentVariableUtil.findExecutableInPathOnAnyOS("xlings")?.let { return it }
        val exe = if (SystemInfo.isWindows) "xlings.exe" else "xlings"
        return File(System.getProperty("user.home"), ".xlings/subos/current/bin/$exe").takeIf { it.isFile }
    }

    private fun tell(project: Project, installed: Boolean, message: String) {
        if (project.isDisposed) return
        group().createNotification("mcppls", message, if (installed) NotificationType.INFORMATION else NotificationType.WARNING).notify(project)
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
