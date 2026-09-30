package community.mcpp.mcppls

import com.intellij.openapi.extensions.ExtensionPointName
import com.intellij.openapi.project.Project
import com.intellij.openapi.project.guessProjectDir
import java.io.File

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
        return loaded || project.guessProjectDir()?.findChild("CMakeLists.txt") != null || cmakeListsOnDisk(project)
    }

    // The file system itself, for a project whose root the VFS has not refreshed yet: just opened, the first files can
    // reach fileOpened before the VFS lists the root's children (CI run 36782178214: no CMakeLists.txt, so no notice).
    private fun cmakeListsOnDisk(project: Project): Boolean =
        project.basePath?.let { File(it, "CMakeLists.txt").isFile } ?: false
}
