// The QuickPick itself (0.0.10 plan C-13.3): it draws what `cacheHub.ts` models, fetches the report
// through the language client, runs the entries through `vscode.commands`, and keeps the drill-down
// one Esc away. A sweep shows `busy`; the receipt replaces the "last sweep" line in place.
import * as vscode from 'vscode';
import { CacheDetail, CxxCacheStatus } from './cacheSegment';
import { drillDownItems, entryLabel, hubItems, hubTitle, HubItem } from './cacheHub';
import { parseSweepResult, rememberCacheDetail, SERVER_SWEEP_CACHE_COMMAND, sweepResultText } from './cacheSweep';

interface ClientLike {
    sendRequest: (method: string, params: unknown, token?: vscode.CancellationToken) => Thenable<unknown>;
    initializeResult?: { capabilities?: { executeCommandProvider?: { commands?: readonly string[] } } };
}

export interface HubContext {
    client: ClientLike | undefined;
    /** The coarse numbers the last status notification carried (the fallback when the detail request fails). */
    coarse: CxxCacheStatus | undefined;
}

/** One `cxxModules/cache` round trip: `{ roots: [...] }`, one report per root (S3 5.7). */
export async function fetchCacheReport(client: ClientLike | undefined, token?: vscode.CancellationToken): Promise<CacheDetail | undefined> {
    if (!client) return undefined;
    try {
        const answer = (await client.sendRequest('cxxModules/cache', {}, token)) as { roots?: CacheDetail[] } | undefined;
        const root = answer?.roots?.[0];
        if (root) rememberCacheDetail(root);
        return root;
    } catch {
        return undefined;   // an old server answers MethodNotFound: the coarse numbers carry the card
    }
}

async function runEntry(item: HubItem & { kind: 'entry' }): Promise<void> {
    if (!item.action) return;
    if (item.action.external) return;   // the view never opens a browser itself; the command does
    await vscode.commands.executeCommand(item.action.command, ...(item.action.arguments ?? []));
}

async function sweep(pick: vscode.QuickPick<vscode.QuickPickItem>, client: ClientLike | undefined, dryRun: boolean): Promise<void> {
    pick.busy = true;
    pick.ignoreFocusOut = true;
    try {
        const answer = await client?.sendRequest('workspace/executeCommand', {
            command: SERVER_SWEEP_CACHE_COMMAND,
            arguments: [{ dryRun }],
        });
        const result = parseSweepResult(answer);
        const receipt = `${dryRun ? '$(eye) ' : '$(clear-all) '}${sweepResultText(result)}`;
        const cacheGroup = pick.items.find((item) => item.label.startsWith('$(clear-all)'));
        pick.items = pick.items.map((item) => (item === cacheGroup ? { ...item, description: receipt } : item));
        await fetchCacheReport(client);   // the numbers the receipt left behind
    } catch (error) {
        void vscode.window.showErrorMessage(`Sweep failed: ${error instanceof Error ? error.message : String(error)}`);
    } finally {
        pick.busy = false;
        pick.ignoreFocusOut = false;
    }
}

/** Opens the hub. `coarse` carries what the status bar already knows; the detail is fetched fresh. */
export async function openCacheHub(context: HubContext): Promise<void> {
    if (context.coarse === undefined && !context.client) {
        void vscode.window.showWarningMessage('The C++ Modules server is not running, so there is no cache to look at.');
        return;
    }
    const detail = await fetchCacheReport(context.client);
    const pick = vscode.window.createQuickPick();
    pick.title = detail ? hubTitle(detail) : 'C++ Modules — 缓存';
    pick.matchOnDescription = false;
    pick.matchOnDetail = false;
    pick.buttons = [{ iconPath: new vscode.ThemeIcon('eye'), tooltip: '预演：先看要删多少，不删' }];

    const draw = (current: CacheDetail | undefined): void => {
        if (!current) return;
        pick.items = hubItems(current, { canSweep: context.client?.initializeResult?.capabilities?.executeCommandProvider?.commands?.includes(SERVER_SWEEP_CACHE_COMMAND) === true }).map(
            (item) =>
                item.kind === 'separator'
                    ? { label: `─ ${item.label} ─`, kind: vscode.QuickPickItemKind.Separator }
                    : { label: entryLabel(item), description: item.description, buttons: 'buttonTitle' in item && item.buttonTitle ? [pick.buttons[0]] : [] },
        );
    };
    draw(detail);

    // The eye button on the sweep entry and the one on the title bar both mean the same: a dry run.
    pick.onDidTriggerItemButton(async ({ item }) => {
        if (!item.label.startsWith('$(clear-all)')) return;
        await sweep(pick, context.client, true);
    });
    pick.onDidTriggerButton(async () => {
        await sweep(pick, context.client, true);
    });
    pick.onDidChangeSelection(async (selected) => {
        const chosen = selected[0];
        if (!chosen) return;
        if (chosen.label.startsWith('$(clear-all)')) {
            await sweep(pick, context.client, false);
            return;
        }
        if (chosen.label.startsWith('$(chevron-right)') || chosen.label.startsWith('$(database)') || chosen.label.startsWith('$(history)')) {
            const fresh = await fetchCacheReport(context.client);
            if (fresh) draw(fresh);
            if (chosen.label.startsWith('$(chevron-right)') && fresh) {
                pick.items = drillDownItems(fresh).map((item) =>
                    item.kind === 'separator'
                        ? { label: `─ ${item.label} ─`, kind: vscode.QuickPickItemKind.Separator }
                        : { label: entryLabel(item), description: item.description },
                );
            }
            return;
        }
        const entry = hubItems(detail ?? ({} as CacheDetail), { canSweep: true }).find((candidate) => candidate.kind === 'entry' && entryLabel(candidate) === chosen.label) as
            | (HubItem & { kind: 'entry' })
            | undefined;
        if (entry?.action) await runEntry(entry);
        if (entry?.refresh && detail) {
            const fresh = await fetchCacheReport(context.client);
            if (fresh) draw(fresh);
        }
        // Keep the hub open for the rest: a menu of actions closes only when the person sends Esc.
    });
    pick.onDidHide(() => pick.dispose());
    pick.show();
}
