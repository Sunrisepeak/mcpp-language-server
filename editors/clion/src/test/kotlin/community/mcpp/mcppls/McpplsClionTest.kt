package community.mcpp.mcppls

import com.intellij.codeInsight.completion.CodeCompletionHandlerBase
import com.intellij.codeInsight.completion.CompletionType
import com.intellij.codeInsight.daemon.impl.HighlightInfo
import com.intellij.codeInsight.lookup.LookupManager
import com.intellij.notification.Notification
import com.intellij.notification.Notifications
import com.intellij.openapi.application.ApplicationManager
import com.intellij.openapi.extensions.ExtensionPointName
import com.intellij.openapi.fileEditor.FileDocumentManager
import com.intellij.openapi.project.Project
import com.intellij.openapi.vfs.LocalFileSystem
import com.intellij.openapi.vfs.VirtualFile
import com.intellij.platform.lsp.api.LspServer
import com.intellij.platform.lsp.api.LspServerManager
import com.intellij.platform.lsp.api.LspServerState
import com.intellij.testFramework.PlatformTestUtil
import com.intellij.testFramework.TestApplicationManager
import com.intellij.testFramework.UsefulTestCase
import com.intellij.testFramework.fixtures.CodeInsightTestFixture
import com.intellij.testFramework.builders.EmptyModuleFixtureBuilder
import com.intellij.testFramework.fixtures.IdeaTestFixtureFactory
import com.intellij.testFramework.fixtures.impl.TempDirTestFixtureImpl
import com.jetbrains.cidr.project.workspace.CidrWorkspaceManager
import java.io.File
import java.nio.file.Files
import java.nio.file.Path
import java.util.concurrent.TimeUnit
import org.eclipse.lsp4j.CompletionItem
import org.eclipse.lsp4j.CompletionList
import org.eclipse.lsp4j.CompletionParams
import org.eclipse.lsp4j.Position
import org.eclipse.lsp4j.jsonrpc.messages.Either

// mcppls inside a running CLion (plugin/CL-3 in the 0.0.8 part 2 plan): the platform's own test
// framework opens a real directory project in a headless CLion and the plugin starts the `mcppls`
// found first on the PATH, which CI sets to the payload it built.
//
// Four things are asserted of an mcpp project: the server is Running within a minute of opening a
// module, a wrong import gets mcppls's diagnostic, `import ` completes a module name from mcppls, and
// closing the project ends the process. Two more of the one-engine rule (McpplsServerSupportProvider):
// a CMake project is CLion's and gets no server, and with the setting on it gets both engines and one
// notice. What CLion's own engine answers for the same file is printed, not asserted.
class McpplsClionTest : UsefulTestCase() {
    private lateinit var fixture: CodeInsightTestFixture
    private lateinit var projectDir: Path
    private var closed = false
    private val settings get() = McpplsSettings.getInstance()
    private val project: Project get() = fixture.project

    override fun setUp() {
        super.setUp()
        TestApplicationManager.getInstance()
        settings.alsoForModelledProjects = false
    }

    override fun tearDown() {
        try {
            settings.alsoForModelledProjects = false
            if (::fixture.isInitialized && !closed) fixture.tearDown()
        } finally {
            super.tearDown()
        }
    }

    private fun open(kind: String) {
        // The fixture builder makes the project in a directory named after it, inside the one it is given.
        val parent = Files.createTempDirectory("mcppls-clion")
        projectDir = parent.resolve(kind)
        File(System.getProperty("mcppls.fixtures"), kind).copyRecursively(projectDir.toFile())
        val builder = IdeaTestFixtureFactory.getFixtureFactory().createFixtureBuilder(kind, parent, true)
        builder.addModule(EmptyModuleFixtureBuilder::class.java).addContentRoot(projectDir.toString())
        fixture = IdeaTestFixtureFactory.getFixtureFactory()
            .createCodeInsightFixture(builder.fixture, TempDirTestFixtureImpl())
        fixture.setUp()
    }

    // Closing the project is tearing its fixture down, which disposes it the way closing the window does.
    private fun closeProject() {
        closed = true
        fixture.tearDown()
    }

    private fun file(relative: String): VirtualFile =
        checkNotNull(LocalFileSystem.getInstance().refreshAndFindFileByNioFile(projectDir.resolve(relative))) { relative }

    private fun servers(): List<LspServer> =
        LspServerManager.getInstance(project).getServersForProvider(McpplsServerSupportProvider::class.java).toList()

    private fun waitFor(what: String, seconds: Int, condition: () -> Boolean) =
        PlatformTestUtil.waitWithEventsDispatching("$what did not happen within $seconds s", condition, seconds)

    private fun running() = servers().any { it.state == LspServerState.Running }

    // The pid of the mcppls this JVM started, from the process table: the plugin gives no handle.
    private fun serverProcess(): ProcessHandle? = ProcessHandle.current().descendants()
        .filter { it.info().command().orElse("").endsWith("mcppls") && it.info().arguments().orElse(emptyArray()).contains("serve") }
        .findFirst().orElse(null)

    private fun highlights(): List<HighlightInfo> = fixture.doHighlighting()

    // What the platform's completion contributor for LSP sends and reads, asked of the server directly
    // (off the EDT, which the platform keeps free for the response to be handled).
    private fun mcpplsCompletions(): List<String> {
        val server = servers().firstOrNull() ?: return emptyList()
        val document = checkNotNull(fixture.editor.document)
        val file = checkNotNull(FileDocumentManager.getInstance().getFile(document))
        val line = document.lineCount - 1
        val params = CompletionParams(server.getDocumentIdentifier(file), Position(line, document.textLength - document.getLineStartOffset(line)))
        val answer = ApplicationManager.getApplication().executeOnPooledThread<Either<List<CompletionItem>, CompletionList>?> {
            server.sendRequestSync(20_000) { it.textDocumentService.completion(params) }
        }.get(30, TimeUnit.SECONDS)
        return (answer?.left ?: answer?.right?.items ?: emptyList()).map { it.label }
    }

    // The platform's own completion, which is CLion's engine's and, when mcppls runs, mcppls's too.
    private fun ideCompletions(): List<String> {
        val editor = fixture.editor
        editor.caretModel.moveToOffset(editor.document.textLength)
        CodeCompletionHandlerBase(CompletionType.BASIC).invokeCompletion(project, editor)
        return LookupManager.getActiveLookup(editor)?.items?.map { it.lookupString } ?: emptyList()
    }

    private fun describe(what: String) {
        println("CLION-ENGINE-EVIDENCE [$what] highlights: " + highlights().map { "${it.severity}:${it.description}" })
        val completions = try {
            ideCompletions().take(20).toString()
        } catch (e: AssertionError) {
            "none within the test framework's limit (${e.message})"
        }
        println("CLION-ENGINE-EVIDENCE [$what] completions: $completions")
    }

    fun testMcppProjectIsMcpplss() {
        open("mcpp")
        fixture.openFileInEditor(file("src/broken.cpp"))

        // (1) the server is Running.
        waitFor("a running mcppls", 60) { running() }
        val process = serverProcess()
        assertNotNull("the mcppls process is a child of this CLion", process)
        println("MCPPLS-RUNNING servers=${servers().size} pid=${process?.pid()}")

        // (2) mcppls's diagnostic for the wrong import.
        waitFor("a diagnostic for `import missing.module`", 120) { highlights().any { it.description?.contains("missing.module") == true } }
        println("MCPPLS-DIAGNOSTICS " + highlights().map { "${it.severity}:${it.description}" })

        // (3) `import hel` completes the project's module.
        fixture.openFileInEditor(file("src/scratch.cpp"))
        var names = emptyList<String>()
        waitFor("a completion of `hello.greet`", 120) { names = mcpplsCompletions(); "hello.greet" in names }
        println("MCPPLS-COMPLETION " + names.take(20))

        // (4) closing the project ends the process.
        closeProject()
        waitFor("the mcppls process to exit", 30) { !process!!.isAlive }
    }

    // What CLion's engine alone says about the same files, for the plan's record (CL-4 evidence).
    fun testWhatClionAnswersAlone() {
        open("mcpp")
        fixture.openFileInEditor(file("src/broken.cpp"))
        waitFor("a running mcppls", 60) { running() }
        LspServerManager.getInstance(project).stopServers(McpplsServerSupportProvider::class.java)
        waitFor("mcppls to stop", 30) { !running() }
        describe("broken.cpp, CLion only")
        fixture.openFileInEditor(file("src/scratch.cpp"))
        describe("scratch.cpp, CLion only")
        println("CLION-WORKSPACE modelled=${ClionModel.models(project)} states=${CidrWorkspaceManager.getInstanceOrNull(project)?.workspaces?.values}")
    }

    // The optional dependency resolves inside CLion, so the workspace API is what answers, and it says
    // the mcpp project is not CLion's.
    fun testWorkspaceProbeIsRegisteredInClion() {
        open("mcpp")
        val probes = ExtensionPointName<ClionWorkspaceProbe>("io.github.sunrisepeak.mcppls.workspaceProbe").extensionList
        assertEquals(listOf(CidrWorkspaceProbe::class.java), probes.map { it.javaClass })
        assertFalse(probes.single().isModelled(project))
    }

    fun testCMakeProjectIsClionsByDefault() {
        open("cmake")
        fixture.openFileInEditor(file("src/main.cpp"))
        fixture.openFileInEditor(file("src/greet/greet.cppm"))
        assertTrue("CLion's project is the CMake one", ClionModel.models(project))
        // A server would start within moments of the first file opening; give it well beyond that.
        Thread.sleep(10_000)
        PlatformTestUtil.dispatchAllEventsInIdeEventQueue()
        assertTrue("no mcppls for a project CLion models", servers().isEmpty())
    }

    fun testCMakeProjectWithTheSwitchOnHasBothEnginesAndOneNotice() {
        settings.alsoForModelledProjects = true
        open("cmake")
        val notices = mutableListOf<Notification>()
        project.messageBus.connect(testRootDisposable)
            .subscribe(Notifications.TOPIC, object : Notifications {
                override fun notify(notification: Notification) {
                    if (notification.groupId == "mcppls") notices += notification
                }
            })
        fixture.openFileInEditor(file("src/main.cpp"))
        fixture.openFileInEditor(file("src/greet/greet.cppm"))
        waitFor("a running mcppls", 60) { running() }
        PlatformTestUtil.dispatchAllEventsInIdeEventQueue()
        assertEquals("one notice per project: $notices", 1, notices.size)
    }
}
