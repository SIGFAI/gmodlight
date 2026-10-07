// GModLight (GooseMcGee, MIT): Garry's Mod's physgun, weapons and spawn menu inside Dying Light. An ASI plugin in Dying
// Light (GModLight.asi, loaded by Ultimate ASI Loader as xinput1_3.dll) starts a hidden 64-bit Garry's Mod itself
// (local gm_flatgrass with world drawing off, +sv_cheats 1, -noworkshop) and composites its frame; a Garry's Mod binary
// module (gmcl_gmodlight_win64.dll) and Lua addon talk to it over the shared memory of shared/bridge.h.
// Rehosted on SIGFAI/gmodlight (standard upstream fusion) from release v0.1.0, which GitHub Actions built from the tag
// commit e3ad71b (run 37231146114). Upstream's install.ps1 layout, every file unchanged:
//   Dying Light: xinput1_3.dll (= ultimate_asi_loader.dll, the official Ultimate ASI Loader v9.7.4 NoPDB x64
//                dinput8.dll), GModLight.asi, GModLight.ini;
//   Garry's Mod: garrysmod/lua/bin/gmcl_gmodlight_win64.dll and the same file as gmsv_gmodlight_win64.dll,
//                garrysmod/addons/gmodlight/ (the Lua addon).
// Only Dying Light is launched: the plugin starts Garry's Mod (found through Steam's libraries) and closes it with it.
//   node library/gmodlight/build.mjs       (outputs: library/lib.mjs)
import { sha256, unzip } from '../../orchestrator/src/recipe.js';
import { card, dl, emit, pinned, player, rawAt, zipAsset } from '../lib.mjs';

const UP = {
  repo: 'https://github.com/GooseMcGee/DyingLight-GMod', tag: 'v0.1.0', commit: 'e3ad71bc2e38141321a9804192d7864ec6c96bde',
  license: 'MIT', authors: ['GooseMcGee', 'akingery15'],
  zip: { file: 'GModLight-0.1.0.zip', sha256: 'f089912ab3366996555e4e0a2abed40903af4dc8ad93e034af6122547af2ce25' }, // = GitHub digest, 2026-10-07
  root: 'GModLight-0.1.0/',
  files: {
    'files/GModLight.asi': 'c296486eda893af046007c34a2a325034bb4bf6b0b12a4a4a4e94299dfd5e134',
    'files/gmcl_gmodlight_win64.dll': '36e07abda93ef36c24eabe04414743bd597d8b2bc7910b063f8d5b1f1563f32f',
    'files/ultimate_asi_loader.dll': '031a3e5576d91dce1e438d36b9a3d462c7334ab4791990a8ff1e3ddc0e132daf', // = UAL v9.7.4 NoPDB x64 dinput8.dll
  },
};
const UAL = { name: 'Ultimate ASI Loader', version: '9.7.4', repo: 'https://github.com/ThirteenAG/Ultimate-ASI-Loader', tag: 'v9.7.4', license: 'MIT' };
const MINHOOK = { name: 'MinHook', repo: 'https://github.com/TsudaKageyu/minhook', commit: '8af6b4acae5a9388fd742b56fa79ece89d96f823', license: 'BSD-2-Clause' }; // submodule pin at the tag
const ID = 'gmodlight', VERSION = '0.1.0', NAME = 'GModLight';
const TAGLINE = 'Garry\'s Mod\'s physgun, weapons and spawn menu inside Dying Light: fling zombies instead of fighting them.';

const all = unzip(await pinned(`${UP.repo}/releases/download/${UP.tag}/${UP.zip.file}`, UP.zip.sha256));
const up = new Map(all.filter(e => !e.name.endsWith('/')).map(e => [e.name.replace(/\\/g, '/').slice(UP.root.length), e.data]));
for (const [f, h] of Object.entries(UP.files)) if (!up.has(f) || sha256(up.get(f)) !== h) throw new Error(`${UP.zip.file}: ${f} missing or not the reviewed build`);
const licenses = [
  { name: 'GModLight/LICENSE.txt', data: up.get('LICENSE') },
  { name: 'GModLight/THIRD_PARTY_NOTICES.md', data: up.get('THIRD_PARTY_NOTICES.md') },
  { name: 'GModLight/LICENSE-UltimateASILoader.txt', data: await rawAt(UAL.repo, UAL.tag, 'license') },
  { name: 'GModLight/LICENSE-MinHook.txt', data: await rawAt(MINHOOK.repo, MINHOOK.commit, 'LICENSE.txt') },
];
const dyinglight = zipAsset(`${ID}-dyinglight.zip`, [
  { name: 'xinput1_3.dll', data: up.get('files/ultimate_asi_loader.dll') },
  { name: 'GModLight.asi', data: up.get('files/GModLight.asi') },
  { name: 'GModLight.ini', data: up.get('files/GModLight.ini') },
  ...licenses,
]);
const addon = [...up.keys()].filter(k => k.startsWith('files/gmodlight/'));
const gmod = zipAsset(`${ID}-gmod.zip`, [
  { name: 'garrysmod/lua/bin/gmcl_gmodlight_win64.dll', data: up.get('files/gmcl_gmodlight_win64.dll') },
  { name: 'garrysmod/lua/bin/gmsv_gmodlight_win64.dll', data: up.get('files/gmcl_gmodlight_win64.dll') },
  ...addon.map(k => ({ name: `garrysmod/addons/gmodlight/${k.slice('files/gmodlight/'.length)}`, data: up.get(k) })),
  { name: 'garrysmod/addons/gmodlight/LICENSE.txt', data: up.get('LICENSE') },
]);
const assets = [dyinglight, gmod];

const make = (urls, set) => ({
  id: `sigf/${ID}`,
  version: VERSION,
  name: NAME,
  tagline: player(ID).tagline ?? TAGLINE,
  how_to_play: player(ID).howToPlay,
  kind: 'passthrough',
  games: [
    { game: 'dyinglight', role: 'host', label: 'Dying Light', engine: 'Dying Light (Chrome Engine 6, D3D11, x64) + ASI plugin GModLight (C++, Ultimate ASI Loader)', apps: { steam: '239140' }, runtime: 'current Steam build (no version pinned upstream; engine_x64_rwdi.dll exports and class names resolved at runtime)' },
    { game: 'gmod', role: 'guest', label: 'Garry\'s Mod', engine: 'Garry\'s Mod x86-64 branch + binary module gmcl_gmodlight + Lua addon', apps: { steam: '4000' }, runtime: '"x86-64 - Chromium + 64-bit binaries" beta branch; started hidden by the Dying Light plugin, never launched by the app' },
  ],
  requires: [
    { id: 'ultimate-asi-loader', version: UAL.version, license: `${UAL.license}, shipped unchanged as xinput1_3.dll`, page: `${UAL.repo}/releases/tag/${UAL.tag}`,
      note: 'loads GModLight.asi in Dying Light; installed into the Dying Light folder by the app' },
    { id: 'gmod-x64', page: 'https://store.steampowered.com/app/4000', note: 'Garry\'s Mod on the 64-bit beta branch: Steam > Garry\'s Mod > Properties > Betas > "x86-64 - Chromium + 64-bit binaries"' },
  ],
  install: [
    { game: 'dyinglight', strategy: 'game-dir-snapshot', files: [
      { src: dyinglight.name, dst: '{game}', unpack: true, contents: dyinglight.contents, ...dl(dyinglight, urls) },
    ] },
    { game: 'gmod', strategy: 'game-dir-snapshot', files: [
      { src: gmod.name, dst: '{game}', unpack: true, contents: gmod.contents, ...dl(gmod, urls) },
    ] },
  ],
  // Dying Light only: GModLight.asi starts Garry's Mod hidden at Dying Light's main menu (a job object closes it with
  // Dying Light). Starting Garry's Mod first would make the plugin reuse it without its launch options.
  launch: [{ game: 'dyinglight', args: [] }],
  files: set.map(a => ({ name: a.name, ...dl(a, urls) })),
  source: {
    repo: UP.repo, license: 'MIT AND BSD-2-Clause', upstream_license: UP.license, tag: UP.tag, commit: UP.commit,
    hosted: `https://github.com/SIGFAI/${ID}`,
    bundled: [
      { name: UAL.name, version: UAL.version, repo: UAL.repo, tag: UAL.tag, license: UAL.license },
      { name: MINHOOK.name, repo: MINHOOK.repo, commit: MINHOOK.commit, license: MINHOOK.license, note: 'statically linked into GModLight.asi and gmcl_gmodlight_win64.dll' },
    ],
  },
  media: {},
  built_by: { author: UP.authors[0], authors: UP.authors, packaged_by: 'SIGF' },
  idea_by: UP.authors[0],
  built_at: '2026-10-07T00:00:00.000Z',
  ...card(UP.repo),
  notes: player(ID).notes,
});

emit({ slug: ID, version: VERSION, assets, make });
