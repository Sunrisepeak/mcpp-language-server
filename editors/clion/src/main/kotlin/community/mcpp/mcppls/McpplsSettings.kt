package community.mcpp.mcppls

import com.intellij.openapi.application.ApplicationManager
import com.intellij.openapi.components.PersistentStateComponent
import com.intellij.openapi.components.State
import com.intellij.openapi.components.Storage

// The one setting: whether mcppls also answers the projects CLion's own C/C++ engine models. Off by
// default, so a file is answered by one engine (McpplsServerSupportProvider, ClionModel).
@State(name = "McpplsSettings", storages = [Storage("mcppls.xml")])
internal class McpplsSettings : PersistentStateComponent<McpplsSettings.State> {
    class State {
        var alsoForModelledProjects: Boolean = false
    }

    private var state = State()

    override fun getState(): State = state

    override fun loadState(loaded: State) {
        state = loaded
    }

    var alsoForModelledProjects: Boolean
        get() = state.alsoForModelledProjects
        set(value) {
            state.alsoForModelledProjects = value
        }

    companion object {
        fun getInstance(): McpplsSettings = ApplicationManager.getApplication().getService(McpplsSettings::class.java)
    }
}
