package community.mcpp.mcppls

import com.intellij.openapi.project.Project
import com.jetbrains.cidr.project.workspace.CidrWorkspaceManager
import com.jetbrains.cidr.project.workspace.CidrWorkspaceState

// The only file that names CLion's C/C++ classes; mcppls-cidr.xml registers it when that plugin is
// present. CidrWorkspaceManager knows every workspace CLion has for the project (CMake, compilation
// database, Makefile, open folder), so one question covers them all. A workspace that is still
// loading counts: CLion is about to answer for it.
internal class CidrWorkspaceProbe : ClionWorkspaceProbe {
    override fun isModelled(project: Project): Boolean =
        CidrWorkspaceManager.getInstanceOrNull(project)?.workspaces?.values
            ?.any { it == CidrWorkspaceState.Loaded || it == CidrWorkspaceState.Loading } == true
}
