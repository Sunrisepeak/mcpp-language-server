package community.mcpp.mcppls

import com.intellij.codeInsight.daemon.impl.analysis.FileHighlightingSetting
import com.intellij.codeInsight.daemon.impl.analysis.HighlightingSettingsPerFile
import com.intellij.notification.NotificationAction
import com.intellij.notification.NotificationGroupManager
import com.intellij.notification.NotificationType
import com.intellij.openapi.application.ApplicationManager
import com.intellij.openapi.fileEditor.FileEditorManager
import com.intellij.openapi.options.ShowSettingsUtil
import com.intellij.openapi.project.Project
import com.intellij.openapi.vfs.VirtualFile
import com.intellij.openapi.vfs.VirtualFileManager
import com.intellij.psi.PsiManager

// One engine per file (design 2026-10-29 §3). The files mcppls answers may be analysed by CLion's own C/C++ engine
// too -- Nova analyses a folder no build system configures, with the default compiler -- so their problems and
// completions come twice. With the user's answer, CLion's engine stops answering them:
//
//   - the file's highlighting level is Syntax (FileHighlightingSetting.SKIP_INSPECTION, the inspection widget's
//     level). Nova's frontend passes the level to its backend, which then reports syntax errors only; the platform's
//     inspections and external tools stop; syntax colouring, line markers and mcppls's diagnostics stay (the LSP
//     highlighting pass does not depend on the level);
//   - completion keeps mcppls's items and live templates (McpplsOnlyCompletion).
//
// Nothing is changed without an answer to the question below or an action the user ran, and every lowered level is
// remembered, so restoring puts back exactly what was there -- except where the user changed it since.
internal object OneEngine {
    // A file mcppls answers: a C or C++ file, of a project CLion does not model unless the setting says otherwise.
    fun answers(project: Project, file: VirtualFile): Boolean =
        file.extension?.lowercase() in CXX_EXTENSIONS &&
            (McpplsSettings.getInstance().alsoForModelledProjects || !ClionModel.models(project))

    fun mcpplsOnly(project: Project, file: VirtualFile): Boolean =
        McpplsProjectSettings.of(project).effective == Engines.MCPPLS_ONLY && answers(project, file)

    // A file mcppls answers was opened and its server started.
    fun fileOpened(project: Project, file: VirtualFile) = onEdt(project) {
        val settings = McpplsProjectSettings.of(project)
        when (settings.effective) {
            Engines.MCPPLS_ONLY -> lower(project, file)
            Engines.ASK -> if (!settings.asked) ask(project)
            Engines.BOTH -> Unit
        }
        OtherLanguageServices.tellOnce(project)
    }

    // The answer, for this project or (everywhere) for every project without one of its own, applied to the files
    // open now; the files opened later follow it as they open.
    fun choose(project: Project, engines: Engines, everywhere: Boolean = false) {
        val settings = McpplsProjectSettings.of(project)
        settings.asked = true
        if (everywhere) {
            McpplsSettings.getInstance().engines = engines
            settings.engines = null
        } else {
            settings.engines = engines
        }
        settingsChanged(project)
    }

    // After any change of the answer (the question, an action, the settings page).
    fun settingsChanged(project: Project) = onEdt(project) {
        if (McpplsProjectSettings.of(project).effective == Engines.MCPPLS_ONLY) {
            FileEditorManager.getInstance(project).openFiles.filter { answers(project, it) }.forEach { lower(project, it) }
        } else {
            restore(project)
        }
    }

    private fun lower(project: Project, file: VirtualFile) {
        val psi = PsiManager.getInstance(project).findFile(file) ?: return
        val levels = HighlightingSettingsPerFile.getInstance(project)
        // A level the user already set lower (None, or Syntax) is theirs and stays.
        if (!levels.shouldInspect(psi)) return
        McpplsProjectSettings.of(project).lowered[file.url] = levels.getHighlightingSettingForRoot(psi).name
        levels.setHighlightingSettingForRoot(psi, FileHighlightingSetting.SKIP_INSPECTION)
    }

    // Every level mcppls lowered goes back to what it was, unless it is no longer the one mcppls set.
    private fun restore(project: Project) {
        val lowered = McpplsProjectSettings.of(project).lowered
        val levels = HighlightingSettingsPerFile.getInstance(project)
        for ((url, before) in lowered.toMap()) {
            val psi = VirtualFileManager.getInstance().findFileByUrl(url)?.let { PsiManager.getInstance(project).findFile(it) }
            val previous = FileHighlightingSetting.entries.firstOrNull { it.name == before }
            if (psi != null && previous != null && levels.getHighlightingSettingForRoot(psi) == FileHighlightingSetting.SKIP_INSPECTION) {
                levels.setHighlightingSettingForRoot(psi, previous)
            }
        }
        lowered.clear()
    }

    // Once per project, the first time a file mcppls answers opens and nothing is decided.
    private fun ask(project: Project) {
        McpplsProjectSettings.of(project).asked = true
        NotificationGroupManager.getInstance().getNotificationGroup("mcppls")
            .createNotification(
                "mcppls",
                "CLion's own C/C++ engine also analyses the files mcppls answers in this project, so problems and " +
                    "completions come twice. Let mcppls answer them alone? Settings | Tools | mcppls changes this later.",
                NotificationType.INFORMATION,
            )
            .addAction(NotificationAction.createSimpleExpiring("Use mcppls only in this project") { choose(project, Engines.MCPPLS_ONLY) })
            .addAction(NotificationAction.createSimpleExpiring("Always use mcppls only") { choose(project, Engines.MCPPLS_ONLY, everywhere = true) })
            .addAction(NotificationAction.createSimpleExpiring("Keep both") { choose(project, Engines.BOTH) })
            .notify(project)
    }

    private fun onEdt(project: Project, action: () -> Unit) =
        ApplicationManager.getApplication().invokeLater({ if (!project.isDisposed) action() }, project.disposed)
}

// Whether this project was asked about the engines; the tests read it.
internal fun askedAboutEngines(project: Project): Boolean = McpplsProjectSettings.of(project).asked

// Other plugins that may run a language server for C and C++ (design 2026-10-29 §3.4). A plugin has no public way to
// stop another plugin's server, so they are named, once per project, with the way to turn them off. Their completion
// and their diagnostics through the platform's inspections are already left out of a file under "mcppls only".
//
// A plugin is known by an extension point it declares, which exists while it is installed and enabled (looking a
// plugin up by its id is the platform's internal API).
internal object OtherLanguageServices {
    data class Candidate(val extensionPoint: String, val name: String)

    val CANDIDATES = listOf(
        Candidate("com.redhat.devtools.lsp4ij.server", "LSP4IJ"),
    )

    fun active(): List<Candidate> =
        CANDIDATES.filter { ApplicationManager.getApplication().extensionArea.hasExtensionPoint(it.extensionPoint) }

    fun tellOnce(project: Project) {
        val told = McpplsProjectSettings.of(project).toldAbout
        for (candidate in active().filter { it.extensionPoint !in told }) {
            told += candidate.extensionPoint
            NotificationGroupManager.getInstance().getNotificationGroup("mcppls")
                .createNotification(
                    "mcppls",
                    "${candidate.name} is installed. If it runs a language server for C or C++ in this project, that server " +
                        "answers the same files as mcppls: turn it off in ${candidate.name}'s settings, or disable the plugin " +
                        "for this project in Settings | Plugins.",
                    NotificationType.INFORMATION,
                )
                .addAction(NotificationAction.createSimpleExpiring("Open Plugins") {
                    ShowSettingsUtil.getInstance().showSettingsDialog(project, "Plugins")
                })
                .notify(project)
        }
    }
}
