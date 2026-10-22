// Pure status wording: the adopted profile and requested build target are different facts.
import { t } from './strings';

export interface SemanticProfile {
    kind: 'build-toolchain' | 'semantic-kit';
    compiler?: string;
    stdlib: string;
    target: string;
}

export interface TargetSelection {
    mode: 'auto' | 'project' | 'explicit';
    requestedTarget: string;
    source: 'configuration' | 'compile-commands' | 'project';
    reason: string;
    evidencePath?: string;
    pending: boolean;
}

export function describeProfile(profile: SemanticProfile | undefined): string {
    if (!profile) return '';
    const compiler = profile.compiler || profile.stdlib || '';
    return [compiler, profile.target].filter(Boolean).join(' · ');
}

export function describeTargetSelection(selection: TargetSelection | undefined): string {
    if (!selection) return '';
    const source = selection.source === 'configuration' ? t('workspace setting')
        : selection.source === 'compile-commands' ? t('existing compile database')
            : t('mcpp defaults');
    const selected = t('Target source: {0}', source);
    const pending = selection.pending
        ? selection.requestedTarget ? t('Target not applied: {0}', selection.requestedTarget) : t('Target selection not applied')
        : '';
    return [selected, pending, selection.reason].filter(Boolean).join(' · ');
}
