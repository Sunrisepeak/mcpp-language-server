package community.mcpp.mcppls

import com.intellij.openapi.options.BoundSearchableConfigurable
import com.intellij.openapi.project.Project
import com.intellij.openapi.ui.DialogPanel
import com.intellij.ui.SimpleListCellRenderer
import com.intellij.ui.dsl.builder.bindItem
import com.intellij.ui.dsl.builder.bindSelected
import com.intellij.ui.dsl.builder.panel

// Settings | Tools | mcppls: which projects mcppls answers, and whether CLion's own engine also answers the files
// mcppls answers (OneEngine) -- for every project, and for this one.
internal class McpplsConfigurable(private val project: Project) :
    BoundSearchableConfigurable("mcppls", "community.mcpp.mcppls.settings") {

    override fun createPanel(): DialogPanel {
        val settings = McpplsSettings.getInstance()
        val here = McpplsProjectSettings.of(project)
        return panel {
            row {
                checkBox("Also for projects CLion models")
                    .bindSelected(settings::alsoForModelledProjects)
                    .comment(
                        "CMake, compilation database and Makefile projects that CLion loads itself are answered by " +
                            "CLion's own C/C++ engine only. With this on, mcppls answers them too. Applies to files " +
                            "opened afterwards.",
                    )
            }
            group("CLion's Engine in the Files mcppls Answers") {
                row("Every project:") {
                    comboBox(listOf(Engines.ASK, Engines.MCPPLS_ONLY, Engines.BOTH), textOf(null))
                        .bindItem({ settings.engines }, { settings.engines = it ?: Engines.ASK })
                }
                row("This project:") {
                    comboBox(listOf(null, Engines.MCPPLS_ONLY, Engines.BOTH), textOf("As every project"))
                        .bindItem({ here.engines }, { here.engines = it; here.asked = true })
                        .comment(
                            "mcppls only: CLion's engine reports syntax errors alone in these files and offers no completion " +
                                "there (the file's highlighting level is Syntax). Both: the two engines answer. Nothing is " +
                                "changed until you choose.",
                        )
                }
            }
        }
    }

    override fun apply() {
        super.apply()
        OneEngine.settingsChanged(project)
    }

    private fun textOf(unset: String?) = SimpleListCellRenderer.create<Engines?> { label, engines, _ ->
        label.text = when (engines) {
            null -> unset ?: ""
            Engines.ASK -> "Ask once per project"
            Engines.MCPPLS_ONLY -> "mcppls only"
            Engines.BOTH -> "Both engines"
        }
    }
}
