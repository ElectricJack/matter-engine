/** Contract error codes the adapter can raise. Runtime-only codes (cancelled,
 *  stale_result, backend_failure, invalid_handle) never originate here. */
export type ViewerErrorCode = 'invalid_input' | 'unsupported_capability' | 'unsupported_version' |
  'missing_dependency' | 'cycle' | 'resource_limit' | 'disposed';

export type DiagnosticSeverity = 'error' | 'warning' | 'info';

export interface ViewerDiagnostic {
  readonly severity: DiagnosticSeverity;
  /** A ViewerErrorCode for errors; a stable label (`fallback`, `ignored`, `derived`) otherwise. */
  readonly code: string;
  readonly message: string;
  /** Asset path of the offending item, e.g. `meshes/prop` or `instances/placed.b/child.a`. */
  readonly path?: string;
  readonly capability?: string;
}

export class ViewerError extends Error {
  readonly code: ViewerErrorCode;
  readonly diagnostics: readonly ViewerDiagnostic[];
  constructor(code: ViewerErrorCode, message: string, diagnostics: readonly ViewerDiagnostic[] = []) {
    super(`${code}: ${message}`);
    this.name = 'ViewerError';
    this.code = code;
    this.diagnostics = Object.freeze([...diagnostics]);
  }
}

/** Collects non-fatal diagnostics and raises the first error with everything seen so far. */
export class DiagnosticLog {
  readonly entries: ViewerDiagnostic[] = [];
  note(severity: Exclude<DiagnosticSeverity, 'error'>, code: string, message: string, extra: {path?: string; capability?: string} = {}): void {
    this.entries.push(Object.freeze({severity, code, message, ...extra}));
  }
  fail(code: ViewerErrorCode, message: string, extra: {path?: string; capability?: string} = {}): never {
    const entry = Object.freeze({severity: 'error' as const, code, message, ...extra});
    throw new ViewerError(code, extra.path ? `${message} (${extra.path})` : message, [...this.entries, entry]);
  }
}
