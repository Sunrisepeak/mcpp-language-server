package community.mcpp.mcppls

import com.intellij.openapi.options.BoundSearchableConfigurable
import com.intellij.openapi.ui.DialogPanel
import com.intellij.ui.dsl.builder.bindSelected
import com.intellij.ui.dsl.builder.panel

// Settings | Tools | mcppls.
internal class McpplsConfigurable : BoundSearchableConfigurable("mcppls", "community.mcpp.mcppls.settings") {
    override fun createPanel(): DialogPanel {
        val settings = McpplsSettings.getInstance()
        return panel {
            row {
                checkBox("Also for projects CLion models")
                    .bindSelected(settings::alsoForModelledProjects)
                    .comment(
                        "CMake, compilation database and Makefile projects that CLion loads itself are answered by " +
                            "CLion's own C/C++ engine only. With this on, mcppls answers them too, so completion and " +
                            "diagnostics come from both engines. Applies to files opened afterwards.",
                    )
            }
        }
    }
}
