package community.mcpp.mcppls

import com.intellij.codeInsight.completion.CodeCompletionHandlerBase
import com.intellij.codeInsight.completion.CompletionType
import com.intellij.codeInsight.daemon.impl.HighlightInfo
import com.intellij.codeInsight.daemon.impl.analysis.FileHighlightingSetting
import com.intellij.codeInsight.daemon.impl.analysis.HighlightingSettingsPerFile
import com.intellij.codeInsight.lookup.LookupElementBuilder
import com.intellij.codeInsight.lookup.LookupElementDecorator
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
import com.intellij.psi.PsiManager
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
// framework opens a real directory project in a headless CLion, whose sandbox holds this machine's
// package of the plugin with the payload CI built inside it (design 2026-10-29 §2), and the plugin
// starts that payload's server: its image, through the platform's runner, as its payload.json says.
//
// Four things are asserted of an mcpp project: the server is Running within a minute of opening a
// module, a wrong import gets mcppls's diagnostic, `import ` completes a module name from mcppls, and
// closing the project ends the process. The one-engine rule (McpplsServerSupportProvider, OneEngine):
// a CMake project is CLion's and gets no server; with the setting on it gets mcppls too and the one
// question; "mcppls only" lowers a file's highlighting level to Syntax and keeps mcppls's diagnostics,
// and restoring puts back what was there. What CLion's own engine answers for the same file is
// printed, not asserted: headless CLion's engine does not answer (design 2026-10-29 §1.4).
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
        settings.engines = Engines.ASK
    }

    override fun tearDown() {
        try {
            settings.alsoForModelledProjects = false
            settings.engines = Engines.ASK
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

    // The pid of the mcppls this JVM started, from the process table: the plugin gives no handle. The
    // payload's server is its image, which the platform's runner runs (`okrun mcppls.okb serve`).
    private fun serverProcess(): ProcessHandle? = ProcessHandle.current().descendants()
        .filter { process ->
            val arguments = process.info().arguments().orElse(emptyArray())
            "serve" in arguments && (process.info().command().orElse("").endsWith("mcppls") || arguments.any { it.endsWith("mcppls.okb") })
        }
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

    // payload.json's `server` is what starts: the image through the runner it names, or a program; a payload built
    // for another system is refused; MCPPLS_SERVER comes first and PATH last.
    fun testThePayloadStartsWhatItsManifestNames() {
        val payload = Files.createTempDirectory("mcppls-payload").toFile()
        try {
            val bin = payload.resolve("bin").also { it.mkdirs() }
            val runner = bin.resolve("okrun").also { it.writeText("runner") }
            val image = bin.resolve("mcppls.okb").also { it.writeText("okb") }
            payload.resolve("payload.json").writeText("""{"platform": "linux-x64", "server": {"path": "bin/mcppls.okb", "runner": "bin/okrun"}}""")
            assertEquals(listOf(runner.path, image.path), serverCommand(payload))
            assertTrue("the runner's executable bit is put back", runner.canExecute())
            assertEquals(Launch.Found(listOf(runner.path, image.path), "payload"), payloadLaunch(payload, "linux-x64"))
            assertTrue("another system's payload is refused", payloadLaunch(payload, "win32-x64") is Launch.Refused)
            image.delete()
            assertNull("an image that is not there is not started", serverCommand(payload))
            assertTrue(payloadLaunch(payload, "linux-x64") is Launch.Refused)
            val program = bin.resolve("mcppls").also { it.writeText("program") }
            payload.resolve("payload.json").writeText("""{"server": {"path": "bin/mcppls"}}""")
            assertEquals(listOf(program.path), serverCommand(payload))

            val elsewhere = File("/usr/bin/env")
            val onPath = { elsewhere }
            assertEquals(Launch.Found(listOf(program.path), "MCPPLS_SERVER"),
                resolveLaunch(mapOf("MCPPLS_SERVER" to program.path), payload, "linux-x64", onPath))
            assertTrue(resolveLaunch(mapOf("MCPPLS_SERVER" to "relative/mcppls"), payload, "linux-x64", onPath) is Launch.Refused)
            assertEquals(Launch.Found(listOf(program.path), "payload"), resolveLaunch(emptyMap(), payload, "linux-x64", onPath))
            assertEquals(Launch.Found(listOf(elsewhere.path), "PATH"), resolveLaunch(emptyMap(), payload.resolve("missing"), "linux-x64", onPath))
            assertEquals(Launch.Missing, resolveLaunch(emptyMap(), null, "linux-x64") { null })
        } finally {
            payload.deleteRecursively()
        }
    }

    // This machine's package carries the payload, and that is what the plugin starts.
    fun testTheSandboxPackageCarriesItsPayload() {
        if (System.getProperty("mcppls.carriesPayload") != "true") {
            println("no payload for this machine (MCPPLS_PAYLOAD unset): the package carries none here")
            return
        }
        val carried = checkNotNull(carriedPayload()) { "the plugin is loaded from a directory" }
        assertTrue("payload.json at ${carried.path}", carried.resolve("payload.json").isFile)
        val launch = resolveLaunch()
        assertTrue("the carried payload starts: $launch", launch is Launch.Found && launch.source == "payload")
    }

    // What "mcppls only" keeps: mcppls's items, through the decorators the platform wraps them in, and live
    // templates; nothing else.
    fun testMcpplsOnlyKeepsMcpplsItems() {
        assertFalse("another engine's item", keeps(LookupElementBuilder.create("other")))
        val marked = markAsMcppls(LookupElementBuilder.create("hello.greet"))
        assertTrue(keeps(marked))
        assertTrue("through a decorator", keeps(LookupElementDecorator.withInsertHandler(marked) { _, _ -> }))
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

        // Nothing was decided, so the project was asked, once, and no level was changed.
        waitFor("the project asked about the engines", 30) { askedAboutEngines(project) }
        assertTrue(McpplsProjectSettings.of(project).lowered.isEmpty())

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
        // mcpp writes no project-root compile_commands.json unless the project asks for one (mcpp
        // 2026.10.26.2), so CLion loads no compilation database here and the project stays mcppls's.
        assertFalse("no root compile_commands.json in an mcpp project", java.io.File(project.basePath!!, "compile_commands.json").exists())
        assertFalse("the mcpp project is not CLion's", ClionModel.models(project))
    }

    // Issue #71, third item: an mcpp project as a person has it after `mcpp build` (CI builds the fixture with the mcpp
    // it tests against, before the suite) is not taken over by CLion's Compilation Database. mcpp keeps the database
    // under target/<triple>/<fingerprint>/ and writes none at the root since 2026.10.26.2, so CLion has nothing to load
    // and mcppls serves the project.
    fun testBuiltMcppProjectIsNotTakenOverByCompilationDatabase() {
        open("mcpp-built")
        val root = projectDir.toFile()
        if (System.getProperty("mcppls.fixturesBuilt") == "true") {
            assertTrue("the fixture was built by mcpp before the suite", File(root, "target").isDirectory)
        } else {
            println("mcpp-built was not built by mcpp here (MCPPLS_FIXTURES_BUILT unset): only the unbuilt project is checked")
        }
        assertFalse("no root compile_commands.json after mcpp build", File(root, "compile_commands.json").exists())
        fixture.openFileInEditor(file("src/main.cpp"))
        waitFor("a running mcppls", 60) { running() }
        assertFalse("CLion's Compilation Database did not take the mcpp project", ClionModel.models(project))
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

    fun testCMakeProjectWithTheSwitchOnHasMcpplsAndOneQuestion() {
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
        // The plugin's own decision to ask about the engines, made once per project. The balloon itself is the
        // platform's to deliver, and headless CLion's message bus did not always within 30 s (CI runs 36782178214,
        // 36797333606), so its count is only held to never more than one.
        waitFor("the project asked about the engines", 30) { askedAboutEngines(project) }
        val settled = System.currentTimeMillis() + 3_000
        while (System.currentTimeMillis() < settled) {
            PlatformTestUtil.dispatchAllEventsInIdeEventQueue()
            Thread.sleep(100)
        }
        assertTrue("one notice per project at most: $notices", notices.size <= 1)
    }

    // "mcppls only": the files mcppls answers go to Syntax, mcppls's diagnostics stay, and restoring puts back what
    // was there -- but not where the user set the level since.
    fun testMcpplsOnlyLowersTheLevelAndRestoringPutsItBack() {
        open("mcpp")
        val levels = HighlightingSettingsPerFile.getInstance(project)
        fun level(relative: String) = levels.getHighlightingSettingForRoot(checkNotNull(PsiManager.getInstance(project).findFile(file(relative))))
        val before = level("src/broken.cpp")

        OneEngine.choose(project, Engines.MCPPLS_ONLY)
        fixture.openFileInEditor(file("src/broken.cpp"))
        waitFor("a running mcppls", 60) { running() }
        waitFor("broken.cpp at Syntax", 30) { level("src/broken.cpp") == FileHighlightingSetting.SKIP_INSPECTION }
        assertEquals(before.name, McpplsProjectSettings.of(project).lowered[file("src/broken.cpp").url])
        waitFor("mcppls's diagnostic at Syntax", 120) { highlights().any { it.description?.contains("missing.module") == true } }

        fixture.openFileInEditor(file("src/scratch.cpp"))
        waitFor("scratch.cpp at Syntax", 30) { level("src/scratch.cpp") == FileHighlightingSetting.SKIP_INSPECTION }
        // The user sets scratch.cpp to None themselves.
        levels.setHighlightingSettingForRoot(checkNotNull(PsiManager.getInstance(project).findFile(file("src/scratch.cpp"))), FileHighlightingSetting.SKIP_HIGHLIGHTING)

        OneEngine.choose(project, Engines.BOTH)
        waitFor("broken.cpp back to its level", 30) { level("src/broken.cpp") == before }
        waitFor("nothing left to restore", 30) { McpplsProjectSettings.of(project).lowered.isEmpty() }
        assertEquals("the user's level stays", FileHighlightingSetting.SKIP_HIGHLIGHTING, level("src/scratch.cpp"))
    }

    // "Always use mcppls only" is the application's answer: a project with none of its own follows it unasked.
    fun testEverywhereIsTheApplicationsAnswer() {
        open("mcpp")
        OneEngine.choose(project, Engines.MCPPLS_ONLY, everywhere = true)
        assertEquals(Engines.MCPPLS_ONLY, settings.engines)
        assertNull("the project follows the application", McpplsProjectSettings.of(project).engines)
        assertEquals(Engines.MCPPLS_ONLY, McpplsProjectSettings.of(project).effective)
        assertTrue(OneEngine.mcpplsOnly(project, file("src/broken.cpp")))
        assertFalse("not a C++ file", OneEngine.mcpplsOnly(project, file("mcpp.toml")))
    }
}
