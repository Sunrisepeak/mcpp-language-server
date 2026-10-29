package community.mcpp.mcppls

import com.intellij.openapi.application.ApplicationManager
import com.intellij.openapi.components.PersistentStateComponent
import com.intellij.openapi.components.Service
import com.intellij.openapi.components.State
import com.intellij.openapi.components.Storage
import com.intellij.openapi.components.StoragePathMacros
import com.intellij.openapi.project.Project

// Whether CLion's own C/C++ engine also answers the files mcppls answers (design 2026-10-29 §3).
internal enum class Engines(val stored: String) {
    // Not decided: nothing is changed, and the project is asked once.
    ASK("ask"),
    MCPPLS_ONLY("mcppls-only"),
    BOTH("both");

    companion object {
        fun of(stored: String?): Engines? = entries.firstOrNull { it.stored == stored }
    }
}

// The application's settings: whether mcppls also answers the projects CLion's own C/C++ engine models (off, so a
// file is answered by one engine: McpplsServerSupportProvider, ClionModel), and the engines of every project that
// has no answer of its own.
@State(name = "McpplsSettings", storages = [Storage("mcppls.xml")])
internal class McpplsSettings : PersistentStateComponent<McpplsSettings.State> {
    class State {
        var alsoForModelledProjects: Boolean = false
        var engines: String = Engines.ASK.stored
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

    var engines: Engines
        get() = Engines.of(state.engines) ?: Engines.ASK
        set(value) {
            state.engines = value.stored
        }

    companion object {
        fun getInstance(): McpplsSettings = ApplicationManager.getApplication().getService(McpplsSettings::class.java)
    }
}

// One project's part, in its workspace file: its own answer, whether it was asked, which files' highlighting level
// mcppls lowered and from what, and the other language services it was told about.
@Service(Service.Level.PROJECT)
@State(name = "McpplsProject", storages = [Storage(StoragePathMacros.WORKSPACE_FILE)])
internal class McpplsProjectSettings : PersistentStateComponent<McpplsProjectSettings.State> {
    class State {
        // "" follows the application's setting.
        var engines: String = ""
        var asked: Boolean = false
        // File URL -> the FileHighlightingSetting it had before mcppls lowered it.
        var lowered: MutableMap<String, String> = mutableMapOf()
        var toldAbout: MutableList<String> = mutableListOf()
    }

    private var state = State()

    override fun getState(): State = state

    override fun loadState(loaded: State) {
        state = loaded
    }

    // The project's own answer; null follows the application's.
    var engines: Engines?
        get() = Engines.of(state.engines)
        set(value) {
            state.engines = value?.stored ?: ""
        }

    var asked: Boolean
        get() = state.asked
        set(value) {
            state.asked = value
        }

    val lowered: MutableMap<String, String> get() = state.lowered
    val toldAbout: MutableList<String> get() = state.toldAbout

    // What applies to this project: its own answer, else the application's.
    val effective: Engines get() = engines ?: McpplsSettings.getInstance().engines

    companion object {
        fun of(project: Project): McpplsProjectSettings = project.getService(McpplsProjectSettings::class.java)
    }
}
