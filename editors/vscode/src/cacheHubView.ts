// The QuickPick itself (0.0.10 plan C-13.3; v2 2026-10-03 UI-9/UI-10): it draws what `cacheHub.ts`
// models, fetches the report through the language client, and dispatches on the ENTRY each item
// carries -- never on the label's icon text, which is how the old code lost the drill-down
// (nothing ever matched `$(chevron-right)`, so "largest modules" was unreachable). Enter runs the
// active entry; the eye button is the dry run; a drill-down has a back button and Esc still closes.
import * as vscode from 'vscode';
import { CacheDetail, CxxCacheStatus } from './cacheSegment';
import { directoryItems, drillDownItems, entryLabel, hubItems, hubTitle, HubEntry, HubItem } from './cacheHub';
import { parseSweepResult, rememberCacheDetail, SERVER_SWEEP_CACHE_COMMAND, sweepResultText } from './cacheSweep';
import { t } from './strings';

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

// The entry travels ON the item, so the event handlers can read what accepting it means without
// parsing its label. `entryLabel` stays the only place that renders icon plus text.
type HubPickItem = vscode.QuickPickItem & { entry?: HubEntry };

function toPickItems(items: HubItem[]): HubPickItem[] {
    return items.map((item) =>
        item.kind === 'separator'
            ? { label: `─ ${item.label} ─`, kind: vscode.QuickPickItemKind.Separator }
            : { label: entryLabel(item), description: item.description,
                buttons: item.buttonTitle ? [{ iconPath: new vscode.ThemeIcon('eye'), tooltip: item.buttonTitle }] : [],
                entry: item },
    );
}

async function runEntry(entry: HubEntry): Promise<void> {
    if (entry.behavior !== 'command' || !entry.action) return;
    if (entry.action.external) return;   // the view never opens a browser itself; the command does
    await vscode.commands.executeCommand(entry.action.command, ...(entry.action.arguments ?? []));
}

/** Opens the hub. `coarse` carries what the status bar already knows; the detail is fetched fresh. */
export async function openCacheHub(context: HubContext): Promise<void> {
    if (context.coarse === undefined && !context.client) {
        void vscode.window.showWarningMessage(t('The C++ Modules server is not running, so there is no cache to look at.'));
        return;
    }
    const detail = await fetchCacheReport(context.client);
    const pick = vscode.window.createQuickPick<HubPickItem>();
    pick.placeholder = t('Type to filter; Enter runs, Esc closes');
    pick.matchOnDescription = true;   // UI-10: the numbers and verbs in descriptions filter too
    const eye = { iconPath: new vscode.ThemeIcon('eye'), tooltip: t('Dry run: see what would go, remove nothing') };
    const back = { iconPath: new vscode.ThemeIcon('arrow-left'), tooltip: t('Back') };
    let drilling: 'modules' | 'directories' | undefined;

    const draw = (current: CacheDetail, receipt?: string): void => {
        drilling = undefined;
        pick.buttons = [eye];
        pick.title = hubTitle(current);
        pick.items = toPickItems(hubItems(current, {
            canSweep: context.client?.initializeResult?.capabilities?.executeCommandProvider?.commands?.includes(SERVER_SWEEP_CACHE_COMMAND) === true,
        }, receipt));
    };
    const drawDrillDown = (which: 'modules' | 'directories', current: CacheDetail): void => {
        drilling = which;
        pick.buttons = [back, eye];
        pick.items = toPickItems(which === 'modules' ? drillDownItems(current) : directoryItems(current));
    };
    if (detail) {
        draw(detail);
    } else {
        pick.title = t('C++ Modules — cache');
        pick.buttons = [eye];
        pick.items = [];
    }

    async function sweep(dryRun: boolean): Promise<void> {
        pick.busy = true;
        pick.ignoreFocusOut = true;
        try {
            const answer = await context.client?.sendRequest('workspace/executeCommand', {
                command: SERVER_SWEEP_CACHE_COMMAND,
                arguments: [{ dryRun }],
            });
            const result = parseSweepResult(answer);
            const receipt = `${dryRun ? '$(eye) ' : '$(clear-all) '}${sweepResultText(result)}`;
            const fresh = await fetchCacheReport(context.client);
            // UI-10: the whole list repaints, so the overview line's numbers change with the receipt.
            if (fresh) draw(fresh, receipt);
        } catch (error) {
            void vscode.window.showErrorMessage(t('Sweep failed: {0}', error instanceof Error ? error.message : String(error)));
        } finally {
            pick.busy = false;
            pick.ignoreFocusOut = false;
        }
    }

    // The eye on the sweep entry and the one on the title bar mean the same: a dry run (D20).
    pick.onDidTriggerItemButton(async ({ item }) => {
        if ((item as HubPickItem).entry?.behavior === 'sweep') await sweep(true);
    });
    pick.onDidTriggerButton(async (button) => {
        if (button === back) {
            const fresh = await fetchCacheReport(context.client);
            if (fresh) draw(fresh);
            return;
        }
        await sweep(true);
    });
    pick.onDidAccept(async () => {
        const entry = pick.activeItems[0]?.entry;
        if (!entry) return;
        if (entry.behavior === 'sweep') {
            await sweep(false);
            return;
        }
        if (entry.behavior === 'detail') {
            const fresh = await fetchCacheReport(context.client);
            if (fresh) drawDrillDown(entry.detail ?? 'modules', fresh);
            return;
        }
        if (entry.behavior === 'refresh') {
            const fresh = await fetchCacheReport(context.client);
            if (fresh) {
                if (drilling === undefined) draw(fresh);
                else drawDrillDown(drilling, fresh);
            }
            return;
        }
        await runEntry(entry);
        // A menu of actions closes only when the person sends Esc; a refresh entry refreshes in place.
        if (entry.behavior === 'command') {
            const fresh = await fetchCacheReport(context.client);
            if (fresh && drilling === undefined) draw(fresh);
        }
    });
    pick.onDidHide(() => pick.dispose());
    pick.show();
}
