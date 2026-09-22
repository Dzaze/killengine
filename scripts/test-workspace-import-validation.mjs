// Automated tests for ui/src/stores/workspaceImportValidation.ts (UX-PIPE-7,
// docs/PHASE_TRACKER.md, 18/09/2026). Not a reimplementation: imports the
// real transpiled module so there is no risk of the test drifting from
// production logic. See scripts/test-workspace-import-validation.ps1 for how
// the transpile step works (same esbuild-vendored-by-Vite pattern already
// used by scripts/test-trainer-dependencies.mjs).
//
// Usage: node scripts/test-workspace-import-validation.mjs <path-to-transpiled-mjs>

import { pathToFileURL } from 'node:url'

const modulePath = process.argv[2]
if (!modulePath) {
  console.error('Usage: node test-workspace-import-validation.mjs <path-to-transpiled-mjs>')
  process.exit(1)
}

const { validateWorkspaceImport } = await import(pathToFileURL(modulePath).href)

let failures = 0

function check(condition, label) {
  if (condition) {
    console.log('OK: ' + label)
  } else {
    failures += 1
    console.error('FAIL: ' + label)
  }
}

function validInvestigationRun(overrides = {}) {
  return {
    id: 1,
    title: 'Run',
    objective: 'Objective',
    processName: 'game.exe',
    startedAt: '2026-09-18T00:00:00.000Z',
    updatedAt: '2026-09-18T00:00:00.000Z',
    status: 'active',
    summary: '',
    steps: [],
    hypotheses: [],
    checkpoints: [],
    ...overrides,
  }
}

function validTrainerFeature(overrides = {}) {
  return {
    id: 1,
    name: 'Infinite HP',
    processName: 'game.exe',
    action: 'write',
    locatorKind: 'absolute',
    address: '7ff7407910e0',
    valueType: 'Int32',
    value: '100',
    enabled: false,
    status: 'idle',
    lastError: '',
    createdAt: '2026-09-18T00:00:00.000Z',
    updatedAt: '2026-09-18T00:00:00.000Z',
    ...overrides,
  }
}

// --- format / version ---------------------------------------------------

{
  const r = validateWorkspaceImport('not json')
  check(r.success === false, 'JSON malformé : rejeté')
  check(r.success === false && r.error.section === 'format' && r.error.reason === 'json_syntax', 'JSON malformé : section/reason corrects')
}

{
  const r = validateWorkspaceImport('42')
  check(r.success === false && r.error.section === 'format' && r.error.reason === 'not_object', 'JSON scalaire (pas un objet) : rejeté')
}

{
  const r = validateWorkspaceImport(JSON.stringify({ version: 2 }))
  check(r.success === false && r.error.section === 'version' && r.error.reason === 'unsupported_version', 'Version non supportée : rejetée')
}

{
  const r = validateWorkspaceImport(JSON.stringify({}))
  check(r.success === false && r.error.section === 'version', 'Version absente : rejetée (traitée comme non supportée)')
}

// --- Reproduction exacte du diagnostic UX-PIPE-7 -------------------------

{
  const raw = JSON.stringify({
    version: 1,
    investigation: { active: { id: 999, objective: 'UX import incomplet' }, archive: [] },
  })
  const r = validateWorkspaceImport(raw)
  check(r.success === false, 'Cas exact du diagnostic (investigation active sans steps) : rejeté, pas juste "aperçu à 1"')
  check(r.success === false && r.error.section === 'investigation' && r.error.reason === 'invalid_active_investigation', 'Cas exact du diagnostic : section/reason corrects')
  check(!('data' in r), 'Cas exact du diagnostic : aucune donnée validée exposée sur rejet (rien à assigner par erreur)')
}

// --- Import partiel : bookmarks seuls, comme cité dans la fiche ----------

{
  const raw = JSON.stringify({ version: 1, bookmarks: { items: [] } })
  const r = validateWorkspaceImport(raw)
  check(r.success === true, 'Import partiel {bookmarks:{items:[]}} : accepté')
  check(r.success === true && r.data.bookmarks?.items.length === 0, 'Import partiel : bookmarks vidés (0 item)')
  check(r.success === true && r.data.investigation === undefined, 'Import partiel : investigation absente du candidat (jamais touchée)')
  check(r.success === true && r.data.trainer === undefined, 'Import partiel : trainer absent du candidat (jamais touché)')
  check(r.success === true && r.data.structures === undefined, 'Import partiel : structures absentes du candidat (jamais touchées)')
  check(r.success === true && r.data.audit === undefined, 'Import partiel : audit absent du candidat (jamais touché)')
  check(r.success === true && r.data.settings === undefined, 'Import partiel : settings absent du candidat (jamais touché)')
}

// --- Export complet valide -------------------------------------------------

{
  const raw = JSON.stringify({
    version: 1,
    exportedAt: '2026-09-18T00:00:00.000Z',
    investigation: { active: validInvestigationRun(), archive: [validInvestigationRun({ id: 2 })] },
    trainer: { features: [validTrainerFeature()] },
    structures: { templates: [] },
    bookmarks: { items: [] },
    audit: { entries: [{ id: 1, time: '00:00', kind: 'scan', title: 'Scan', detail: '', status: 'info' }] },
    settings: { language: 'fr', modelEnabled: true },
    workflowPresets: { lastPresetId: 'preset-a' },
  })
  const r = validateWorkspaceImport(raw)
  check(r.success === true, 'Export complet valide : accepté')
  check(r.success === true && r.data.investigation?.active?.id === 1, 'Export complet : investigation active conservée')
  check(r.success === true && r.data.investigation?.archive.length === 1, 'Export complet : archive conservée')
  check(r.success === true && r.data.trainer?.features.length === 1, 'Export complet : feature Trainer conservée')
  check(r.success === true && r.data.audit?.entries.length === 1, 'Export complet : entrée audit conservée')
  check(r.success === true && r.data.settings?.language === 'fr' && r.data.settings?.modelEnabled === true, 'Export complet : settings partiels conservés tels quels')
  check(r.success === true && r.data.lastPresetId === 'preset-a', 'Export complet : lastPresetId conservé')
}

// --- Première section valide, section suivante invalide : rien n'est appliqué ---

{
  const raw = JSON.stringify({
    version: 1,
    bookmarks: { items: [] }, // valide
    trainer: { features: [{ id: 1, name: 'Broken' }] }, // invalide : champs requis manquants
  })
  const r = validateWorkspaceImport(raw)
  check(r.success === false, 'Section valide suivie d\'une section invalide : tout le résultat est un rejet')
  check(r.success === false && r.error.section === 'trainer', 'Section invalide correctement identifiée malgré une section précédente valide')
  check(!('data' in r), 'Aucune donnée partiellement validée exposée (bookmarks pourtant valides) sur rejet global')
}

// --- Types d'éléments invalides dans chaque section couverte -------------

{
  const r = validateWorkspaceImport(JSON.stringify({ version: 1, trainer: { features: [{ id: 1 }] } }))
  check(r.success === false && r.error.section === 'trainer' && r.error.reason === 'invalid_feature_entry', 'Feature Trainer mal typée : rejetée')
}

{
  const r = validateWorkspaceImport(JSON.stringify({ version: 1, structures: { templates: [{ id: 1, name: 'X' }] } }))
  check(r.success === false && r.error.section === 'structures' && r.error.reason === 'invalid_template_entry', 'Template de structure mal typé : rejeté')
}

{
  const r = validateWorkspaceImport(JSON.stringify({ version: 1, bookmarks: { items: [{ id: 'not-a-number' }] } }))
  check(r.success === false && r.error.section === 'bookmarks' && r.error.reason === 'invalid_bookmark_entry', 'Bookmark mal typé : rejeté')
}

{
  const r = validateWorkspaceImport(JSON.stringify({ version: 1, audit: { entries: [{ id: 1, status: 'not-a-status' }] } }))
  check(r.success === false && r.error.section === 'audit' && r.error.reason === 'invalid_entry', 'Entrée audit mal typée : rejetée')
}

{
  const r = validateWorkspaceImport(JSON.stringify({ version: 1, settings: { modelThreads: 'beaucoup' } }))
  check(r.success === false && r.error.section === 'settings' && r.error.reason === 'invalid_field' && r.error.detail === 'modelThreads', 'Champ settings mal typé : rejeté avec le nom du champ')
}

{
  const r = validateWorkspaceImport(JSON.stringify({ version: 1, investigation: 'not-an-object' }))
  check(r.success === false && r.error.section === 'investigation' && r.error.reason === 'section_not_object', 'Section investigation non-objet : rejetée')
}

// --- Investigation active explicitement null (session sans run actif) ---

{
  const raw = JSON.stringify({ version: 1, investigation: { active: null, archive: [] } })
  const r = validateWorkspaceImport(raw)
  check(r.success === true && r.data.investigation?.active === null, 'investigation.active = null (aucun run actif) : accepté tel quel')
}

if (failures > 0) {
  console.error(`\n${failures} assertion(s) failed.`)
  process.exit(1)
}
console.log('\nAll workspace import validation assertions passed.')
