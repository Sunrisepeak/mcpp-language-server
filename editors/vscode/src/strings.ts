// The extension's user-facing words (plan 2026-10-03 UI-1): English is the source language and the
// zh-cn bundle translates, so the same code shows either language by the display language alone --
// no setting of our own. Deliberately free of `vscode`: the card and the hub are pure modules that
// unit tests render without an editor, and they localize through this indirection instead. The
// extension installs `vscode.l10n.t` at activation (extension.ts); the identity default keeps
// plain-Node tests on the source language.
export type Localizer = (message: string, ...args: (string | number | boolean)[]) => string;

// The {0}-style placeholders vscode.l10n.t substitutes, done here so the default (English, no
// bundle) formats the same way the translated bundle will.
function substitute(message: string, args: (string | number | boolean)[]): string {
    return message.replace(/\{(\d+)\}/g, (_, index) => String(args[Number(index)] ?? ''));
}

let localize: Localizer = (message, ...args) => substitute(message, args);

export function setLocalizer(replacement: Localizer): void {
    localize = replacement;
}

export function t(message: string, ...args: (string | number | boolean)[]): string {
    return localize(message, ...args);
}
