package community.mcpp.mcppls

import com.intellij.codeInsight.completion.CompletionContributor
import com.intellij.codeInsight.completion.CompletionParameters
import com.intellij.codeInsight.completion.CompletionResultSet
import com.intellij.codeInsight.lookup.LookupElement
import com.intellij.codeInsight.lookup.LookupElementDecorator
import com.intellij.codeInsight.template.impl.LiveTemplateLookupElement
import com.intellij.openapi.util.Key
import com.intellij.platform.lsp.api.LspServerManager
import com.intellij.platform.lsp.api.LspServerState
import com.intellij.platform.lsp.api.customization.LspCompletionSupport
import org.eclipse.lsp4j.CompletionItem

// The completion half of "mcppls only" (OneEngine, design 2026-10-29 §3.3): in a file mcppls answers alone, the list
// holds mcppls's items and live templates. This contributor runs first, lets the others run, and passes on what
// they offer only when it is one of those -- so CLion's engine's items, and any other plugin's, are left out. With
// no mcppls running for the project it changes nothing, so a file is never left without completion.
internal class McpplsOnlyCompletion : CompletionContributor() {
    override fun fillCompletionVariants(parameters: CompletionParameters, result: CompletionResultSet) {
        val file = parameters.originalFile.virtualFile ?: return
        val project = parameters.originalFile.project
        if (!OneEngine.mcpplsOnly(project, file) || !mcpplsRunning(project)) return
        result.runRemainingContributors(parameters) { offered ->
            if (keeps(offered.lookupElement)) result.passResult(offered)
        }
        result.stopHere()
    }

    private fun mcpplsRunning(project: com.intellij.openapi.project.Project): Boolean =
        LspServerManager.getInstance(project).getServersForProvider(McpplsServerSupportProvider::class.java)
            .any { it.state == LspServerState.Running }
}

private val MCPPLS_ITEM = Key.create<Boolean>("community.mcpp.mcppls.item")

// mcppls's items, marked where the platform makes them, and the platform's live templates. The platform wraps an
// item in decorators, so the mark is looked for through them.
internal fun keeps(element: LookupElement): Boolean {
    var current: LookupElement? = element
    while (current != null) {
        if (current.getUserData(MCPPLS_ITEM) == true || current is LiveTemplateLookupElement) return true
        current = (current as? LookupElementDecorator<*>)?.delegate
    }
    return false
}

// The platform's completion of mcppls's items, each marked as mcppls's.
internal class McpplsCompletionSupport : LspCompletionSupport() {
    override fun createLookupElement(parameters: CompletionParameters, item: CompletionItem): LookupElement? =
        super.createLookupElement(parameters, item)?.also { it.putUserData(MCPPLS_ITEM, true) }
}

// The mark, for the tests' own items.
internal fun markAsMcppls(element: LookupElement): LookupElement = element.also { it.putUserData(MCPPLS_ITEM, true) }
