package community.mcpp.mcppls

import com.intellij.openapi.extensions.ExtensionPointName
import com.intellij.openapi.project.Project
import com.intellij.openapi.project.guessProjectDir

// Whether CLion's own C/C++ engine models a project: a CMake, compilation database or Makefile
// workspace that CLion has loaded. Those projects are CLion's; mcppls answers the rest.
//
// The answer comes from CLion's workspace API, which lives in the C/C++ plugin. It is reached through
// an extension point that mcppls-cidr.xml fills only when that plugin is there (an optional
// dependency), so a missing or renamed class there costs the probe, not the plugin.
internal interface ClionWorkspaceProbe {
    fun isModelled(project: Project): Boolean
}

internal object ClionModel {
    private val probes = ExtensionPointName<ClionWorkspaceProbe>("io.github.sunrisepeak.mcppls.workspaceProbe")

    // A CMakeLists.txt at the root counts as modelled on its own. It is what CLion offers to load
    // when the project opens, before its workspace exists, and all there is to go on when the API is
    // not there.
    fun models(project: Project): Boolean {
        val loaded = probes.extensionList.any { probe -> runCatching { probe.isModelled(project) }.getOrDefault(false) }
        return loaded || project.guessProjectDir()?.findChild("CMakeLists.txt") != null
    }
}
