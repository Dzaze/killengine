// Automated tests for ui/src/stores/workspaceHistoryScheduling.ts
// (UX-PRODUIT-13, docs/PHASE_TRACKER.md). Imports the real transpiled module
// so there is no risk of drift from production logic -- see
// scripts/test-workspace-history-scheduling.ps1 for the transpile step (same
// esbuild-vendored-by-Vite pattern as test-workspace-import-validation.ps1).
//
// Usage: node scripts/test-workspace-history-scheduling.mjs <path-to-transpiled-mjs>

import { pathToFileURL } from 'node:url'

const modulePath = process.argv[2]
if (!modulePath) {
  console.error('Usage: node test-workspace-history-scheduling.mjs <path-to-transpiled-mjs>')
  process.exit(1)
}

const { computeFingerprint, shouldCaptureAutomatic, diffRecoverableSnapshots } = await import(pathToFileURL(modulePath).href)

let failures = 0

function check(condition, label) {
  if (condition) {
    console.log('OK: ' + label)
  } else {
    failures += 1
    console.error('FAIL: ' + label)
  }
}

function snapshotJson(overrides = {}) {
  return JSON.stringify({
    version: 1,
    exportedAt: new Date().toISOString(),
    lastPresetId: 'preset-a',
    investigation: { active: null, archive: [] },
    trainer: { features: [] },
    structures: { templates: [] },
    bookmarks: { items: [] },
    audit: { entries: [] },
    ...overrides,
  })
}

// --- computeFingerprint -----------------------------------------------

check(
  computeFingerprint(snapshotJson()) === computeFingerprint(snapshotJson()),
  'computeFingerprint: identical content (different exportedAt) yields the same fingerprint',
)

check(
  computeFingerprint(snapshotJson({ lastPresetId: 'preset-a' })) !== computeFingerprint(snapshotJson({ lastPresetId: 'preset-b' })),
  'computeFingerprint: a real content change yields a different fingerprint',
)

check(
  computeFingerprint('not json') === computeFingerprint('not json'),
  'computeFingerprint: invalid JSON is handled deterministically without throwing',
)

const keyOrderA = JSON.stringify({ version: 1, exportedAt: 'x', lastPresetId: 'p', trainer: { features: [] } })
const keyOrderB = JSON.stringify({ trainer: { features: [] }, lastPresetId: 'p', exportedAt: 'y', version: 1 })
check(
  computeFingerprint(keyOrderA) === computeFingerprint(keyOrderB),
  'computeFingerprint: key order does not affect the fingerprint',
)

// --- shouldCaptureAutomatic ---------------------------------------------

check(
  shouldCaptureAutomatic({ lastFingerprint: 'a', currentFingerprint: 'a', lastAutoSaveAtMs: 0, nowMs: 100_000 }) === false,
  'shouldCaptureAutomatic: no real change (same fingerprint) never captures',
)

check(
  shouldCaptureAutomatic({ lastFingerprint: 'a', currentFingerprint: 'b', lastAutoSaveAtMs: 0, nowMs: 100_000 }) === true,
  'shouldCaptureAutomatic: real change, no recent auto-save -> captures',
)

check(
  shouldCaptureAutomatic({ lastFingerprint: 'a', currentFingerprint: 'b', lastAutoSaveAtMs: 90_000, nowMs: 100_000 }) === false,
  'shouldCaptureAutomatic: real change but within the 30s throttle -> does not capture',
)

check(
  shouldCaptureAutomatic({ lastFingerprint: 'a', currentFingerprint: 'b', lastAutoSaveAtMs: 60_000, nowMs: 100_000 }) === true,
  'shouldCaptureAutomatic: real change, exactly at the 30s throttle boundary -> captures',
)

check(
  shouldCaptureAutomatic({ lastFingerprint: null, currentFingerprint: 'first', lastAutoSaveAtMs: 0, nowMs: 100_000 }) === true,
  'shouldCaptureAutomatic: first-ever fingerprint (null baseline) captures',
)

// --- diffRecoverableSnapshots ---------------------------------------------

const before = {
  version: 1,
  lastPresetId: 'p',
  trainer: { features: [{ id: 1, name: 'A', enabled: false, status: 'idle', lastError: '' }] },
  structures: { templates: [] },
  bookmarks: { items: [] },
  audit: { entries: [] },
  investigation: { active: null, archive: [] },
}
const afterAdded = {
  ...before,
  trainer: { features: [
    { id: 1, name: 'A', enabled: false, status: 'idle', lastError: '' },
    { id: 2, name: 'B', enabled: false, status: 'idle', lastError: '' },
  ] },
}
const trainerDiffAdded = diffRecoverableSnapshots(before, afterAdded).find((d) => d.section === 'trainer')
check(
  trainerDiffAdded.items.length === 1 && trainerDiffAdded.items[0].kind === 'added' && trainerDiffAdded.items[0].id === '2',
  'diffRecoverableSnapshots: a new trainer feature is reported as added by id',
)

const afterRemoved = { ...before, trainer: { features: [] } }
const trainerDiffRemoved = diffRecoverableSnapshots(before, afterRemoved).find((d) => d.section === 'trainer')
check(
  trainerDiffRemoved.items.length === 1 && trainerDiffRemoved.items[0].kind === 'removed' && trainerDiffRemoved.items[0].id === '1',
  'diffRecoverableSnapshots: a removed trainer feature is reported as removed by id',
)

const afterModified = {
  ...before,
  trainer: { features: [{ id: 1, name: 'A renamed', enabled: false, status: 'idle', lastError: '' }] },
}
const trainerDiffModified = diffRecoverableSnapshots(before, afterModified).find((d) => d.section === 'trainer')
check(
  trainerDiffModified.items.length === 1 && trainerDiffModified.items[0].kind === 'modified' && trainerDiffModified.items[0].id === '1',
  'diffRecoverableSnapshots: same id, different content is reported as modified (not added+removed)',
)

const identical = diffRecoverableSnapshots(before, before)
check(
  identical.every((d) => d.items.length === 0),
  'diffRecoverableSnapshots: identical snapshots produce no diff items in any section',
)

const beforeNoActive = { ...before, investigation: { active: null, archive: [] } }
const afterActive = { ...before, investigation: { active: { id: 5, title: 'Run', objective: '', processName: '', startedAt: '', updatedAt: '', status: 'active', summary: '', steps: [], hypotheses: [], checkpoints: [] }, archive: [] } }
const investigationDiff = diffRecoverableSnapshots(beforeNoActive, afterActive).find((d) => d.section === 'investigation')
check(
  investigationDiff.items.some((item) => item.kind === 'added' && item.id === '5'),
  'diffRecoverableSnapshots: a newly active investigation is reported as added',
)

// Never match two entries by name/label alone: different ids, same name -> add + remove, not a "modified".
const beforeNamed = { ...before, trainer: { features: [{ id: 1, name: 'Same Name', enabled: false, status: 'idle', lastError: '' }] } }
const afterRenamedId = { ...before, trainer: { features: [{ id: 2, name: 'Same Name', enabled: false, status: 'idle', lastError: '' }] } }
const renamedIdDiff = diffRecoverableSnapshots(beforeNamed, afterRenamedId).find((d) => d.section === 'trainer')
check(
  renamedIdDiff.items.length === 2
    && renamedIdDiff.items.some((i) => i.kind === 'removed' && i.id === '1')
    && renamedIdDiff.items.some((i) => i.kind === 'added' && i.id === '2'),
  'diffRecoverableSnapshots: same name but different id is never coalesced into a single "modified" entry',
)

if (failures > 0) {
  console.error(`\n${failures} failure(s).`)
  process.exit(1)
}
console.log('\nAll workspace history scheduling assertions passed.')
