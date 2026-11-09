import com.intellij.driver.client.Remote
import com.intellij.driver.client.service
import com.intellij.driver.client.utility
import com.intellij.driver.model.OnDispatcher
import com.intellij.driver.sdk.Document
import com.intellij.driver.sdk.FileEditorManager
import com.intellij.driver.sdk.Project
import com.intellij.driver.sdk.VirtualFile
import com.intellij.driver.sdk.invokeAction
import com.intellij.driver.sdk.ui.ui
import com.intellij.driver.sdk.openEditor
import com.intellij.driver.sdk.singleProject
import com.intellij.driver.sdk.waitForProjectOpen
import com.intellij.ide.starter.ci.CIServer
import com.intellij.ide.starter.ci.NoCIServer
import com.intellij.ide.starter.di.di
import com.intellij.ide.starter.driver.engine.runIdeWithDriver
import com.intellij.ide.starter.ide.IdeDistributionFactory
import com.intellij.ide.starter.ide.IdeInstaller
import com.intellij.ide.starter.models.IdeInfo
import com.intellij.ide.starter.models.TestCase
import com.intellij.ide.starter.plugins.PluginConfigurator
import com.intellij.ide.starter.project.LocalProjectInfo
import com.intellij.ide.starter.runner.Starter
import com.intellij.platform.testFramework.teamCity.TeamCityReporter.SyntheticTestKind
import org.junit.jupiter.api.Assertions.*
import kotlin.time.Duration.Companion.minutes
import org.junit.jupiter.api.Test
import org.kodein.di.DI
import org.kodein.di.bindSingleton
import org.kodein.di.direct
import org.kodein.di.instance
import java.nio.file.Files
import java.nio.file.Path
import java.security.MessageDigest
import java.util.concurrent.TimeUnit

private const val PLUGIN_ID = "io.github.sunrisepeak.mcppls"
private const val PROVIDER = "community.mcpp.mcppls.McpplsServerSupportProvider"

@Remote("com.intellij.openapi.application.ApplicationInfo")
interface ApplicationInfoFactory { fun getInstance(): ApplicationInfoRemote }
@Remote("com.intellij.openapi.application.ApplicationInfo")
interface ApplicationInfoRemote {
    fun getFullVersion(): String
    fun getBuild(): BuildNumberRemote
}
@Remote("com.intellij.openapi.util.BuildNumber")
interface BuildNumberRemote { fun asString(): String }
@Remote("com.intellij.openapi.extensions.PluginId")
interface PluginIdFactory { fun getId(id: String): PluginIdRemote }
@Remote("com.intellij.openapi.extensions.PluginId")
interface PluginIdRemote
@Remote("com.intellij.ide.plugins.PluginManagerCore")
interface PluginManagerFactory { fun getPlugin(id: PluginIdRemote): PluginDescriptorRemote? }
@Remote("com.intellij.ide.plugins.IdeaPluginDescriptor")
interface PluginDescriptorRemote { fun getVersion(): String }

@Remote("com.intellij.openapi.fileEditor.FileEditorManager")
interface FileEditorManagerFactory { fun getInstance(project: Project): FileEditorManager }
@Remote("com.intellij.openapi.vfs.LocalFileSystem")
interface LocalFileSystemFactory { fun getInstance(): LocalFileSystemRemote }
@Remote("com.intellij.openapi.vfs.LocalFileSystem")
interface LocalFileSystemRemote { fun refreshAndFindFileByPath(path: String): VirtualFile? }

@Remote("com.intellij.openapi.extensions.ExtensionPointName")
interface ExtensionPointFactory { fun create(name: String): ExtensionPointRemote }
@Remote("com.intellij.openapi.extensions.ExtensionPointName")
interface ExtensionPointRemote { fun getExtensionList(): List<ProviderRemote> }
@Remote("java.lang.Object")
interface ProviderRemote { fun getClass(): JavaClassRemote }
@Remote("java.lang.Class")
interface JavaClassRemote { fun getName(): String }
@Remote("community.mcpp.mcppls.McpplsProjectSettings", plugin = PLUGIN_ID)
interface ProjectSettingsRemote { fun getEffective(): EngineChoiceRemote }
@Remote("community.mcpp.mcppls.Engines", plugin = PLUGIN_ID)
interface EngineChoiceRemote { fun getStored(): String }
@Remote("com.intellij.openapi.project.DumbService")
interface DumbServiceFactory { fun getInstance(project: Project): DumbServiceRemote }
@Remote("com.intellij.openapi.project.DumbService")
interface DumbServiceRemote { fun isDumb(): Boolean }
@Remote("com.intellij.platform.lsp.api.LspServerManager")
interface LspManagerFactory { fun getInstance(project: Project): LspManagerRemote }
@Remote("com.intellij.platform.lsp.api.LspServerManager")
interface LspManagerRemote { fun getServersForProvider(provider: JavaClassRemote): List<LspServerRemote> }
@Remote("com.intellij.platform.lsp.api.LspServer")
interface LspServerRemote {
    fun getState(): LspStateRemote
    fun getInitializeResult(): InitializeResultRemote?
    fun getDescriptor(): LspDescriptorRemote
}
@Remote("com.intellij.platform.lsp.api.LspServerState")
interface LspStateRemote { fun name(): String }
@Remote("org.eclipse.lsp4j.InitializeResult")
interface InitializeResultRemote { fun getServerInfo(): ServerInfoRemote }
@Remote("org.eclipse.lsp4j.ServerInfo")
interface ServerInfoRemote { fun getVersion(): String }
@Remote("com.intellij.platform.lsp.api.LspServerDescriptor")
interface LspDescriptorRemote { fun createCommandLine(): CommandLineRemote }
@Remote("com.intellij.execution.configurations.GeneralCommandLine")
interface CommandLineRemote {
    fun getExePath(): String
    fun getParametersList(): ParametersRemote
}
@Remote("com.intellij.execution.configurations.ParametersList")
interface ParametersRemote { fun getList(): List<String> }

@Remote("com.intellij.codeInsight.daemon.impl.DaemonCodeAnalyzerImpl")
interface HighlightsFactory {
    fun getHighlights(document: Document, severity: SeverityRemote?, project: Project): List<HighlightRemote>
}
@Remote("com.intellij.lang.annotation.HighlightSeverity")
interface SeverityRemote
@Remote("com.intellij.codeInsight.daemon.impl.HighlightInfo")
interface HighlightRemote { fun getDescription(): String? }
@Remote("com.intellij.codeInsight.lookup.LookupManager")
interface LookupManagerFactory { fun getInstance(project: Project): LookupManagerRemote }
@Remote("com.intellij.codeInsight.lookup.LookupManager")
interface LookupManagerRemote { fun getActiveLookup(): LookupRemote? }
@Remote("com.intellij.codeInsight.lookup.impl.LookupImpl")
interface LookupRemote {
    fun getItems(): List<LookupItemRemote>
    fun setCurrentItem(item: LookupItemRemote)
}
@Remote("com.intellij.codeInsight.lookup.LookupElement")
interface LookupItemRemote {
    fun getLookupString(): String
    fun getUserData(key: KeyRemote): Boolean?
}
@Remote("com.intellij.openapi.util.Key")
interface KeyFactory { fun findKeyByName(name: String): KeyRemote? }
@Remote("com.intellij.openapi.util.Key")
interface KeyRemote

class PackagedPluginTest {
    private enum class Stage {
        FIXTURE, IDE_LAUNCH, PROJECT_OPEN, IDE_IDENTITY, PROVIDER, INITIAL_EDITOR,
        CMAKE_OWNERSHIP, CMAKE_CLOSE, SERVER_RUNNING, SERVER_IDENTITY, CARRIED_PAYLOAD,
        MCPPLS_ONLY_SELECTION, IMPORT_DIAGNOSTIC, CPP_TYPE_DIAGNOSTIC,
        MODULE_COMPLETION, COMPLETION_ORIGIN, COMPLETION_INSERTION,
        NAV_INDEXING, NAV_FOCUS, NAV_DEFINITION, MODULE_CLOSE, STRICT_IDE_CLOSE, COMPLETE,
    }
    private enum class ProjectKind { MODULES, CMAKE }
    private var stage = Stage.FIXTURE
    private var projectKind = ProjectKind.MODULES
    private val passedChecks = linkedSetOf<String>()
    private fun passed() { passedChecks += "${projectKind.name}.${stage.name}" }
    init {
        di = DI {
            extend(di)
            bindSingleton<CIServer>(overrides = true) {
                object : CIServer by NoCIServer {
                    override fun reportTestFailure(testName: String, message: String, details: String, linkToLogs: String?, kind: SyntheticTestKind, generifyTestName: Boolean) {
                        throw AssertionError("IDE failure: $testName: $message")
                    }
                }
            }
        }
    }

    private fun await(what: String, seconds: Long = 60, condition: () -> Boolean) {
        val deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(seconds)
        while (System.nanoTime() < deadline) {
            if (condition()) { passed(); return }
            Thread.sleep(100)
        }
        fail<Unit>(what)
    }

    @Test
    fun packagedLanguageFeaturesWorkInTheStableIde() {
        val version = System.getProperty("mcppls.clion.ideVersion") ?: error("ideVersion is required")
        val plugin = Path.of(System.getProperty("mcppls.clion.pluginZip") ?: error("pluginZip is required"))
        val zipSha = MessageDigest.getInstance("SHA-256").digest(Files.readAllBytes(plugin)).joinToString("") { "%02x".format(it) }
        val output = Path.of(System.getProperty("mcppls.clion.output") ?: "build/stable-runtime").toAbsolutePath()
        Files.createDirectories(output)
        Files.deleteIfExists(output.resolve("result.json"))
        var actualBuild = ""
        var serverVersion = ""
        var serverCarrier = ""
        var pluginVersion = ""
        var modulesPhasePassed = false
        var cmakePhasePassed = false
        var strictIdeRun = false
        try {
            for (kind in listOf("modules", "cmake")) {
                projectKind = if (kind == "modules") ProjectKind.MODULES else ProjectKind.CMAKE
                stage = Stage.FIXTURE
                val fixture = Files.createTempDirectory(output, "project space 项目-")
                Files.createDirectories(fixture.resolve("src/greet"))
                if (kind == "modules") Files.writeString(fixture.resolve("mcpp.toml"), "[package]\nname = \"stable-language-test\"\nversion = \"0.1.0\"\n")
                else Files.writeString(fixture.resolve("CMakeLists.txt"), "cmake_minimum_required(VERSION 3.28)\nproject(stable_language_test LANGUAGES CXX)\nadd_executable(app src/main.cpp)\n")
                Files.writeString(fixture.resolve("src/main.cpp"), "import hello.greet;\nint main() { return hello::answer(); }\n")
                Files.writeString(fixture.resolve("src/greet/greet.cppm"), "export module hello.greet;\nexport namespace hello { int answer() { return 42; } }\n")
                // Two real modules keep explicit completion open for selection;
                // a sole candidate can be inserted automatically by the platform.
                Files.writeString(fixture.resolve("src/greet/other.cppm"), "export module hello.other;\nexport int other() { return 7; }\n")
                Files.writeString(fixture.resolve("src/scratch.cpp"), "import hel")
                Files.writeString(fixture.resolve("src/broken.cpp"), "import missing.module;\n")
                Files.writeString(fixture.resolve("src/type-error.cpp"), "int value() { return \"wrong\"; }\n")
                val localHome = System.getProperty("mcppls.clion.ideHome")?.let(Path::of)
                var ide = IdeInfo(productCode = "CL", platformPrefix = "CLion", executableFileName = "clion", fullName = "CLion", version = version, buildType = "release")
                if (localHome != null) ide = ide.copy(getInstaller = {
                    object : IdeInstaller {
                        override suspend fun install(ideInfo: IdeInfo) =
                            Files.readString(localHome.resolve("build.txt")).trim().removePrefix("CL-") to
                                di.direct.instance<IdeDistributionFactory>().installIDE(localHome, "clion")
                    }
                })
                val context = Starter.newContext("mcppls-stable-$version-$kind-${System.currentTimeMillis()}", TestCase(ide, LocalProjectInfo(fixture)))
                PluginConfigurator(context).installPluginFromPath(plugin)
                context.applyVMOptionsPatch {
                    withXmx(2048)
                    addSystemProperty("idea.trust.all.projects", true)
                    addSystemProperty("ide.show.tips.on.startup.default.value", false)
                    // Synthetic fixtures only: retain exact RPC ordering when strict IDE errors fail.
                    addSystemProperty("idea.log.debug.categories", "#com.intellij.platform.lsp.impl.logging.LanguageServiceLoggerService")
                }
                passed()
                stage = Stage.IDE_LAUNCH
                context.runIdeWithDriver(runTimeout = 3.minutes).useDriverAndCloseIde {
                    passed()
                    stage = Stage.PROJECT_OPEN
                    waitForProjectOpen()
                    passed()
                    withContext {
                        stage = Stage.IDE_IDENTITY
                        val app = utility<ApplicationInfoFactory>().getInstance()
                        assertEquals(version, app.getFullVersion(), "the requested stable IDE actually runs")
                        actualBuild = app.getBuild().asString()
                        pluginVersion = requireNotNull(utility<PluginManagerFactory>().getPlugin(utility<PluginIdFactory>().getId(PLUGIN_ID))).getVersion()
                        passed()
                        val project = singleProject()
                        stage = Stage.PROVIDER
                        val provider = utility<ExtensionPointFactory>().create("com.intellij.platform.lsp.serverSupportProvider")
                            .getExtensionList().single { it.getClass().getName() == PROVIDER }.getClass()
                        passed()
                        val manager = utility<LspManagerFactory>().getInstance(project)
                        val vfs = utility<LocalFileSystemFactory>().getInstance()
                        val editors = utility<FileEditorManagerFactory>().getInstance(project)
                        fun dismissTrialTip() {
                            // A normal click closes the SDK's introductory trial tooltip on the EDT.
                            // Leaving it open hits a platform-only off-EDT balloon disposal at project close.
                            val close = ui.x("//div[@visible_text='Close' and contains(@javaclass,'ActionLink')]")
                            if (close.present()) close.click()
                        }
                        stage = Stage.INITIAL_EDITOR
                        openEditor(requireNotNull(vfs.refreshAndFindFileByPath(fixture.resolve(if (kind == "cmake") "src/main.cpp" else "src/broken.cpp").toString())))
                        passed()
                        if (kind == "cmake") {
                            stage = Stage.CMAKE_OWNERSHIP
                            Thread.sleep(5000)
                            assertTrue(manager.getServersForProvider(provider).isEmpty(), "CMake stays with CLion's engine by default")
                            passed()
                            stage = Stage.CMAKE_CLOSE
                            dismissTrialTip()
                            invokeAction("CloseProject")
                            passed()
                        } else {
                            stage = Stage.SERVER_RUNNING
                            await("the packaged plugin starts a running language server") { manager.getServersForProvider(provider).any { it.getState().name() == "Running" } }
                            stage = Stage.SERVER_IDENTITY
                            val server = manager.getServersForProvider(provider).single()
                            serverVersion = requireNotNull(server.getInitializeResult()).getServerInfo().getVersion()
                            assertTrue(serverVersion.matches(Regex("\\d+\\.\\d+\\.\\d+")), "the real server completes initialize")
                            assertEquals(System.getProperty("mcppls.clion.expectedServerVersion") ?: Regex("^\\d+\\.\\d+\\.\\d+").find(pluginVersion)?.value, serverVersion, "the expected carried server actually runs")
                            passed()
                            stage = Stage.CARRIED_PAYLOAD
                            val command = server.getDescriptor().createCommandLine()
                            assertTrue(command.getExePath().contains("/payload/"), "the installed ZIP supplies its server runner")
                            val imagePath = command.getParametersList().getList().firstOrNull { it.endsWith("/payload/bin/mcppls.okb") }
                            val nativeServer = command.getExePath().endsWith("/payload/bin/mcppls")
                            assertTrue(imagePath != null || nativeServer, "the carried server entry is started")
                            serverCarrier = if (imagePath != null) "image" else "native"
                            val nativeExecutable = if (nativeServer) Path.of(command.getExePath()).toRealPath().toString() else null
                            val serverProcess = ProcessHandle.current().descendants().use { processes ->
                                processes.filter { p ->
                                    val args = p.info().arguments().orElse(emptyArray())
                                    "serve" in args && (if (imagePath != null) imagePath in args else p.info().command().orElse("") == nativeExecutable)
                                }.findFirst().orElse(null)
                            }
                            assertNotNull(serverProcess, "the carried server is an IDE descendant")
                            passed()
                            stage = Stage.MCPPLS_ONLY_SELECTION
                            invokeAction("community.mcpp.mcppls.UseMcpplsOnly")
                            await("the actual project mode is mcppls only") {
                                service<ProjectSettingsRemote>(project).getEffective().getStored() == "mcppls-only"
                            }
                            stage = Stage.IMPORT_DIAGNOSTIC
                            val brokenDocument = requireNotNull(editors.getSelectedTextEditor()).getDocument()
                            await("mcppls's missing import diagnostic reaches the actual editor", 120) {
                                withContext(OnDispatcher.EDT) { withReadAction { utility<HighlightsFactory>().getHighlights(brokenDocument, null, project).any { it.getDescription()?.contains("missing.module") == true } } }
                            }
                            stage = Stage.CPP_TYPE_DIAGNOSTIC
                            openEditor(requireNotNull(vfs.refreshAndFindFileByPath(fixture.resolve("src/type-error.cpp").toString())))
                            val typeErrorDocument = requireNotNull(editors.getSelectedTextEditor()).getDocument()
                            await("the carried compiler engine diagnoses a real C++ type error", 120) {
                                withContext(OnDispatcher.EDT) { withReadAction {
                                    utility<HighlightsFactory>().getHighlights(typeErrorDocument, null, project).any {
                                        val description = it.getDescription().orEmpty()
                                        description.contains("cannot initialize", ignoreCase = true) || description.contains("cannot convert", ignoreCase = true)
                                    }
                                } }
                            }
                            stage = Stage.MODULE_COMPLETION
                            openEditor(requireNotNull(vfs.refreshAndFindFileByPath(fixture.resolve("src/scratch.cpp").toString())))
                            val scratch = requireNotNull(editors.getSelectedTextEditor())
                            withContext(OnDispatcher.EDT) { scratch.getCaretModel().moveToOffset(scratch.getDocument().getText().length) }
                            // Asked once the project is indexed, and asked again while a popup without
                            // the module stands: a popup shown before the server answered is not its
                            // answer (the first ask raced indexing on CLion 2026.2).
                            await("project indexing settles before completion", 120) { !utility<DumbServiceFactory>().getInstance(project).isDumb() }
                            invokeAction("CodeCompletion")
                            var asked = System.nanoTime()
                            val lookups = utility<LookupManagerFactory>().getInstance(project)
                            var module: LookupItemRemote? = null
                            await("the actual completion popup offers the project's module", 120) {
                                withContext(OnDispatcher.EDT) { module = lookups.getActiveLookup()?.getItems()?.firstOrNull { it.getLookupString() == "hello.greet" } }
                                if (module == null && System.nanoTime() - asked > TimeUnit.SECONDS.toNanos(10)) {
                                    withContext(OnDispatcher.EDT) { scratch.getCaretModel().moveToOffset(scratch.getDocument().getText().length) }
                                    invokeAction("CodeCompletion")
                                    asked = System.nanoTime()
                                }
                                module != null
                            }
                            stage = Stage.COMPLETION_ORIGIN
                            val mark = requireNotNull(utility<KeyFactory>().findKeyByName("community.mcpp.mcppls.item"))
                            assertEquals(true, requireNotNull(module).getUserData(mark), "the popup item came from mcppls")
                            passed()
                            stage = Stage.COMPLETION_INSERTION
                            withContext(OnDispatcher.EDT) { lookups.getActiveLookup()!!.setCurrentItem(requireNotNull(module)) }
                            invokeAction("EditorChooseLookupItem")
                            await("choosing completion inserts the module name") { scratch.getDocument().getText().contains("import hello.greet") }
                            stage = Stage.NAV_INDEXING
                            openEditor(requireNotNull(vfs.refreshAndFindFileByPath(fixture.resolve("src/main.cpp").toString())))
                            await("project indexing settles before navigation", 120) { !utility<DumbServiceFactory>().getInstance(project).isDumb() }
                            stage = Stage.NAV_FOCUS
                            dismissTrialTip()
                            val mainComponent = ui.x("//div[@class='EditorComponentImpl' and @accessiblename='Editor for main.cpp']")
                            mainComponent.click()
                            await("the main editor owns keyboard focus") { mainComponent.isFocusOwner() }
                            stage = Stage.NAV_DEFINITION
                            val main = requireNotNull(editors.getSelectedTextEditor())
                            withContext(OnDispatcher.EDT) { main.getCaretModel().moveToOffset(main.getDocument().getText().indexOf("answer") + 2) }
                            // Exercise the focused editor's real action context; null-component
                            // ActionManager invocation also reaches native backend readiness checks.
                            @Suppress("OPT_IN_USAGE")
                            mainComponent.keyboard { hotKey(java.awt.event.KeyEvent.VK_CONTROL, java.awt.event.KeyEvent.VK_B) }
                            await("navigation opens the exported module declaration", 120) { editors.getSelectedTextEditor()?.getDocument()?.getText()?.contains("export module hello.greet") == true }
                            stage = Stage.MODULE_CLOSE
                            dismissTrialTip()
                            invokeAction("CloseProject")
                            await("closing the project ends its language server", 30) { !serverProcess!!.isAlive }
                        }
                        stage = Stage.STRICT_IDE_CLOSE
                    }
                }
                passed()
                if (kind == "modules") modulesPhasePassed = true else cmakePhasePassed = true
            }
            strictIdeRun = true
            stage = Stage.COMPLETE
            passed()
        } finally {
            // Failure reports retain actual public versions and completed phases, never logs or paths.
            val passedJson = passedChecks.joinToString(prefix = "[", postfix = "]") { "\"$it\"" }
            Files.writeString(output.resolve("result.json"), """{"ideVersion":"$version","ideBuild":"$actualBuild","pluginZipSha256":"$zipSha","pluginVersion":"$pluginVersion","serverVersion":"$serverVersion","serverCarrier":"$serverCarrier","stage":"${stage.name}","projectKind":"${projectKind.name}","passedChecks":$passedJson,"strictIdeRun":$strictIdeRun,"modulesPhasePassed":$modulesPhasePassed,"cmakePhasePassed":$cmakePhasePassed,"verifiedModuleChecks":${if (modulesPhasePassed) "[\"startup\",\"carriedPayload\",\"diagnostic\",\"cxxTypeDiagnostic\",\"moduleCompletion\",\"completionInsertion\",\"navigation\",\"projectCloseCleanup\"]" else "[]"}}""" + "\n")
        }
    }
}
