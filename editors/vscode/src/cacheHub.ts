// The QuickPick hub's items (0.0.10 plan C-13.3, D15, D20): every entry opens with its codicon, the
// entries sit in five separator groups (缓存 / 清理 / 维护 / 日志 / 开源), and the one primary
// action is the first of the 清理 group. Pure: the view (cacheHubView.ts) only draws this.
import { CacheDetail, sizeText } from './cacheSegment';
import { COPY_AGENT_PROMPT_COMMAND, REVEAL_CACHE_DIRECTORY_COMMAND, SWEEP_WORKSPACE_CACHE_COMMAND } from './cacheSweep';

export interface HubAction {
    command: string;
    /** Arguments the command receives; a sweep carries the category list, a link the URL. */
    arguments?: unknown[];
    /** The command opens a browser (only these go through `openExternal`, and only on click). */
    external?: boolean;
}

export interface HubEntry {
    icon: string;
    label: string;
    description?: string;
    /** A command the entry runs when accepted. Absent on data lines: accepting them refreshes. */
    action?: HubAction;
    /** Data lines say so; accepting one asks for the report again. */
    refresh?: boolean;
}

export type HubItem = { kind: 'separator'; label: string } | ({ kind: 'entry' } & HubEntry);

export interface HubCapabilities {
    /** The server advertises `mcppls.sweepCache`; otherwise the whole 清理 group stays out. */
    canSweep: boolean;
}

const SWEEP_BUTTON_TITLE = '预演（先看要删多少，不删）';

/** The main entry, with the `$(eye)` dry-run button the view hangs on it (D20). */
export function sweepEntry(): HubEntry & { buttonTitle: string } {
    return {
        icon: '$(clear-all)',
        label: '清理缓存（不重启、不重编）',
        description: '先预演？看条目右侧的按钮',
        buttonTitle: SWEEP_BUTTON_TITLE,
        action: { command: SWEEP_WORKSPACE_CACHE_COMMAND, arguments: [{ dryRun: false }] },
    };
}

/** All entries of the hub, in display order. Engine names never appear (D17). */
export function hubItems(detail: CacheDetail, caps: HubCapabilities): HubItem[] {
    const items: HubItem[] = [];
    items.push({ kind: 'separator', label: '缓存' });
    items.push({
        kind: 'entry',
        icon: '$(database)',
        label: `${sizeText(detail.bytes)} / ${sizeText(detail.limits.perWorkspace)}`,
        description: detail.limits.over ? '超过预算' : `副本 ${sizeText(detail.copies.bytes)} · 实例 ${sizeText(detail.instances.bytes)} · 垃圾箱 ${sizeText(detail.trash?.bytes ?? 0)}`,
        refresh: true,
    });
    if (detail.lastSweep && detail.lastSweep.at > 0) {
        items.push({
            kind: 'entry',
            icon: '$(history)',
            label: `上次清理释放 ${sizeText(detail.lastSweep.freedBytes)}（${detail.lastSweep.files} 个文件）`,
            description: detail.lastSweep.failed ? `${detail.lastSweep.failed} 个未能删除` : '不重启、不重编',
            refresh: true,
        });
    }
    if (caps.canSweep) {
        items.push({ kind: 'separator', label: '清理' });
        items.push({ kind: 'entry', ...sweepEntry() });
    }
    items.push({ kind: 'separator', label: '维护' });
    items.push({ kind: 'entry', icon: '$(debug-restart)', label: '重启引擎', action: { command: 'mcppls.restartClangd' } });
    items.push({ kind: 'entry', icon: '$(refresh)', label: '重启服务端', action: { command: 'mcppls.restartServer' } });
    items.push({ kind: 'entry', icon: '$(trash)', label: '重置缓存…', description: '会重新编译模块', action: { command: 'mcppls.resetWorkspaceCache' } });
    items.push({ kind: 'separator', label: '日志' });
    items.push({ kind: 'entry', icon: '$(file-zip)', label: '抓取日志（含报告）', action: { command: 'mcppls.exportDiagnosticBundle' } });
    items.push({ kind: 'entry', icon: '$(output)', label: '打开日志', action: { command: 'mcppls.showLogs' } });
    items.push({ kind: 'entry', icon: '$(folder-opened)', label: '打开日志目录', action: { command: REVEAL_CACHE_DIRECTORY_COMMAND, arguments: ['logs'] } });
    items.push({ kind: 'entry', icon: '$(folder-opened)', label: '打开缓存目录', action: { command: REVEAL_CACHE_DIRECTORY_COMMAND, arguments: ['cache'] } });
    items.push({ kind: 'separator', label: '开源' });
    items.push({
        kind: 'entry',
        icon: '$(copy)',
        label: '复制 Agent 提示词',
        description: '粘给本地 agent，只读排障——日志不出本机',
        action: { command: COPY_AGENT_PROMPT_COMMAND },
    });
    items.push({ kind: 'entry', icon: '$(github)', label: '新建 issue…', description: '预填版本与环境', action: { command: 'mcppls.newCacheIssue' } });
    items.push({ kind: 'entry', icon: '$(repo)', label: '打开开源仓库', action: { command: 'mcppls.openRepository' } });
    items.push({ kind: 'entry', icon: '$(book)', label: '打开文档', action: { command: 'mcppls.openDocumentation' } });
    items.push({ kind: 'entry', icon: '$(gear)', label: '打开设置', action: { command: 'mcppls.openCacheSettings' } });
    return items;
}

/** The one read-only drill-down (D20): the largest modules and the issue prompt, Esc returns. */
export function drillDownItems(detail: CacheDetail): HubItem[] {
    const items: HubItem[] = [{ kind: 'separator', label: '最大模块' }];
    const largest = detail.largest ?? [];
    if (largest.length === 0) {
        items.push({ kind: 'entry', icon: '$(circle-slash)', label: '还没有缓存的模块' });
    }
    for (const module of largest.slice(0, 5)) {
        items.push({
            kind: 'entry',
            icon: '$(file-binary)',
            label: `${module.module} · ${sizeText(module.bytes)}`,
            description: `${module.copies} 份`,
        });
    }
    items.push({ kind: 'separator', label: '开源' });
    items.push({
        kind: 'entry',
        icon: '$(copy)',
        label: '复制 issue 提示词',
        description: '让 agent 把结论整理成草稿，先给人看再发',
        action: { command: 'mcppls.copyIssuePrompt' },
    });
    return items;
}

/** What the title says, engine-free (D17): state, then the plan's scale when the server gave it. */
export function hubTitle(detail: Pick<CacheDetail, 'state' | 'project' | 'plan'>): string {
    const scale = detail.plan && detail.plan.units > 0 ? ` · ${detail.plan.units} units · ${detail.plan.modules} modules` : '';
    return `C++ Modules — ${detail.project.name}（${detail.state}${scale}）`;
}

/** The entry's visible label with its icon, the way the view draws it. */
export function entryLabel(entry: HubEntry): string {
    return `${entry.icon} ${entry.label}`;
}
