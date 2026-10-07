// Shared helpers of the library build scripts (library/<slug>/build.mjs): pinned downloads, release assets with their
// zip contents, and the three outputs of one recipe. Recipe shape: docs/PLATFORM-SPEC.md section 4 ("Upstream
// fusions", "Upstream fetch"); the fusion reference is orchestrator/scripts/package-fusion.mjs (imported, not edited).
//
// Outputs of `node library/<slug>/build.mjs [--fixture]`:
//   library/<slug>/mashup.json                      planned URLs (SIGFAI/<slug> release assets, or upstream release files)
//   orchestrator/test/out/library/<slug>/           file:// variant + every asset, for `cargo run --example install`
//   app/src-tauri/tests/fixtures/<slug>/ (--fixture) file:// variant with an offline pack, only when the assets are
//                                                     under 2 MB in total (FIXTURE_MAX)
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { sha256, unzip, zip } from '../orchestrator/src/recipe.js';
import { FIXTURE_MAX } from '../orchestrator/scripts/package-fusion.mjs';

const here = path.dirname(fileURLToPath(import.meta.url));
export const ROOT = path.resolve(here, '..');
export const CACHE = path.join(ROOT, 'orchestrator', 'test', 'out', 'library-cache');
const UA = { 'User-Agent': 'SIGFAI/mod-orchestrator (sigf.ai)' };

// BepInEx 5 (MIT; its bundled UnityDoorstop winhttp.dll is LGPL-2.1), the official x64 build, shipped unchanged as a release asset of the mashup's SIGFAI repo.
export const BEPINEX = {
  id: 'bepinex', version: '5.4.23.5', file: 'BepInEx_win_x64_5.4.23.5.zip',
  url: 'https://github.com/BepInEx/BepInEx/releases/download/v5.4.23.5/BepInEx_win_x64_5.4.23.5.zip',
  sha256: '82f9878551030f54657792c0740d9d51a09500eeae1fba21106b0c441e6732c4', // GitHub release digest, checked 2026-10-05
  repo: 'https://github.com/BepInEx/BepInEx', commit: '57f1fb859bd4d0264cd2a59074d0e96c6a492a33', license: 'MIT AND LGPL-2.1',
};

/** A pinned file: from the cache when its sha256 matches, else downloaded and checked. */
export async function pinned(url, expect, name = url.split('/').pop()) {
  const file = path.join(CACHE, name);
  if (fs.existsSync(file) && sha256(fs.readFileSync(file)) === expect) return fs.readFileSync(file);
  const res = await fetch(url, { headers: UA, redirect: 'follow' });
  if (!res.ok) throw new Error(`${url}: HTTP ${res.status}`);
  const data = Buffer.from(await res.arrayBuffer());
  const got = sha256(data);
  if (got !== expect) throw new Error(`${name}: sha256 ${got}, pinned ${expect}`);
  fs.mkdirSync(CACHE, { recursive: true });
  fs.writeFileSync(file, data);
  return data;
}

/** A file of an upstream repo at a pinned commit (LICENSE, notices). */
export async function rawAt(repo, commit, file) {
  const res = await fetch(`https://raw.githubusercontent.com/${repo.slice('https://github.com/'.length)}/${commit}/${file}`, { headers: UA });
  if (!res.ok) throw new Error(`${file} at ${commit}: HTTP ${res.status}`);
  return Buffer.from(await res.arrayBuffer());
}

/** Every file entry of a zip with its sha256 (folders left out, as the app's engine does). */
export const contentsOf = (data) => unzip(data).filter(e => !e.name.endsWith('/')).map(e => ({ path: e.name.replace(/\\/g, '/'), sha256: sha256(e.data) }));

/** A release asset. `zipped`: it is a zip the app unpacks, so its contents are listed. upstream: the URL it is
 *  fetched from instead of the SIGFAI release (source.fetch "upstream"). */
export function asset(name, data, { zipped = false, upstream = null } = {}) {
  return { name, data, sha256: sha256(data), size: data.length, ...(zipped ? { contents: contentsOf(data) } : {}), ...(upstream ? { upstream } : {}) };
}

/** Our own zip of [{ name, data }], entries sorted. */
export function zipAsset(name, entries) {
  return asset(name, zip([...entries].sort((a, b) => a.name.localeCompare(b.name))), { zipped: true });
}

/** Replaces fields of a .mrpack's modrinth.index.json (name, summary), every other entry unchanged. */
export function renamePack(data, set) {
  return zip(unzip(data).map(e => e.name === 'modrinth.index.json'
    ? { name: e.name, data: Buffer.from(JSON.stringify({ ...JSON.parse(e.data.toString('utf8')), ...set }, null, 2)) }
    : e));
}

/** The download fields of an asset for a URL map. */
export const dl = (a, urls) => ({ url: urls[a.name], sha256: a.sha256, size: a.size });

/**
 * Writes the outputs of one recipe. make(urls, assets) returns the recipe for asset name -> URL. assets: the planned
 * assets; fixtureAssets: the same set built offline (no Modrinth downloads in a pack), or null for no fixture.
 */
export function emit({ slug, version, assets, fixtureAssets = null, make, argv = process.argv.slice(2) }) {
  const hosted = `https://github.com/SIGFAI/${slug}`;
  const planned = Object.fromEntries(assets.map(a => [a.name, a.upstream ?? `${hosted}/releases/download/v${version}/${a.name}`]));
  const json = (r) => JSON.stringify(r, null, 2) + '\n';
  const lib = path.join(ROOT, 'library', slug);
  fs.writeFileSync(path.join(lib, 'mashup.json'), json(make(planned, assets)));

  const local = (dir, set) => {
    fs.rmSync(dir, { recursive: true, force: true });
    fs.mkdirSync(dir, { recursive: true });
    const urls = {};
    for (const a of set) {
      fs.writeFileSync(path.join(dir, a.name), a.data);
      urls[a.name] = pathToFileURL(path.join(dir, a.name)).href;
    }
    const r = make(urls, set);
    r.built_at = '2026-10-05T00:00:00.000Z';
    fs.writeFileSync(path.join(dir, 'mashup.json'), json(r));
  };
  const out = path.join(ROOT, 'orchestrator', 'test', 'out', 'library', slug);
  local(out, assets);
  const total = assets.reduce((n, a) => n + a.size, 0);
  console.log(`${slug}: ${assets.map(a => `${a.name} (${a.size} B${a.upstream ? ', upstream' : ''})`).join(', ')}; ${total} B in total`);
  console.log(`  library/${slug}/mashup.json (planned URLs), ${path.relative(ROOT, out)}/ (file:// variant)`);
  if (argv.includes('--fixture') && fixtureAssets) {
    const size = fixtureAssets.reduce((n, a) => n + a.size, 0);
    if (size >= FIXTURE_MAX) console.log(`  no fixture: ${size} B is over ${FIXTURE_MAX} B`);
    else {
      local(path.join(ROOT, 'app', 'src-tauri', 'tests', 'fixtures', slug), fixtureAssets);
      console.log(`  app/src-tauri/tests/fixtures/${slug}/ (${size} B, offline pack)`);
    }
  }
}

export const NOTES_MAX = 6, NOTE_MAX = 300, TAGLINE_MAX = 140;
// how_to_play (docs/PLATFORM-SPEC.md section 4): 2 to 5 one-line in-game lines of at most 160 chars, plain text, never
// install steps. Same rule as checkHowToPlay in orchestrator/src/recipe.js on platform-app (INSTALLISH copied as is).
export const HOW_TO_PLAY_MIN = 2, HOW_TO_PLAY_MAX = 5, HOW_TO_PLAY_LINE_MAX = 160;
const INSTALLISH = /\binstall|\bmods? folder|\.jar\b|\.pk3\b|\bfabric\b|\baddons\b|server\.cfg|(^|\s)-(file|game)\b|\bunzip|\bdrop (the|this|it)\b|\bcopy\b[^.]*\b(into|next to|folder|files?)\b|\.tmod\b|\bmy games\b/i;
/**
 * The player-facing words of a recipe, from library/<slug>/player.json: `howToPlay` (the recipe's `how_to_play`, shown
 * under "How to play": the goal, how to start, the keys), `notes` (shown under "Before you play": only what is left,
 * what to install by hand and the limits), an optional `tagline` for the card, and `issues` when upstream has no issue
 * tracker: false (no bug link) or "hosted" (bugs go to the SIGFAI copy's tracker). Kept apart from the build so they
 * change without touching an asset (a pack's summary keeps the build's own TAGLINE): library/renote.mjs rewrites only
 * these fields of mashup.json.
 */
export function player(slug) {
  const p = JSON.parse(fs.readFileSync(path.join(ROOT, 'library', slug, 'player.json'), 'utf8'));
  const bad = (why) => { throw new Error(`library/${slug}/player.json: ${why}`); };
  const h = p.howToPlay;
  if (!Array.isArray(h) || h.length < HOW_TO_PLAY_MIN || h.length > HOW_TO_PLAY_MAX) bad(`howToPlay: ${HOW_TO_PLAY_MIN} to ${HOW_TO_PLAY_MAX} lines`);
  for (const l of h) {
    if (typeof l !== 'string' || !l.trim() || l !== l.trim() || l.length > HOW_TO_PLAY_LINE_MAX) bad(`a howToPlay line must be 1 to ${HOW_TO_PLAY_LINE_MAX} chars, trimmed`);
    // eslint-disable-next-line no-control-regex
    if (/[\u0000-\u001f\u007f]|\*\*|^#|\]\(/.test(l)) bad(`howToPlay: plain one-line text, no markdown: ${l}`);
    if (INSTALLISH.test(l)) bad(`howToPlay: install steps belong in notes: ${l}`);
  }
  if (!Array.isArray(p.notes) || !p.notes.length || p.notes.length > NOTES_MAX) bad(`notes: 1 to ${NOTES_MAX} lines`);
  for (const n of p.notes) if (typeof n !== 'string' || !n.trim() || n.length >= NOTE_MAX || /[\r\n]/.test(n)) bad(`a note must be one line under ${NOTE_MAX} chars`);
  if (/^How it works:/.test(p.notes[0])) bad('"How it works" is the first howToPlay line now, not a note');
  if (p.tagline !== undefined && (typeof p.tagline !== 'string' || !p.tagline.trim() || p.tagline.length > TAGLINE_MAX)) bad(`tagline: up to ${TAGLINE_MAX} chars`);
  if (p.issues !== undefined && p.issues !== false && p.issues !== 'hosted') bad('issues: false (no tracker) or "hosted" (the tracker of the SIGFAI copy)');
  return { howToPlay: h, notes: p.notes, tagline: p.tagline ?? null, issues: p.issues ?? null };
}

/** Card fields every library recipe carries (the app ignores them today). */
export const card = (upstreamRepo) => ({ status: 'beta', issues: `${upstreamRepo}/issues` });
