// Pulled out of conflicts.ts so these two exact button labels have one
// source of truth reachable from test/runTest.ts too. runTest.ts runs as a
// plain Node process, before any VS Code extension host exists, and cannot
// require('vscode') -- so it cannot import anything from conflicts.ts
// itself, which does. This module imports nothing and is safe from both sides.
export const DISABLE = 'Disable in this workspace';
export const KEEP = 'Keep both';
