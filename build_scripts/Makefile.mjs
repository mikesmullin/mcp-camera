#!/usr/bin/env node
// Build driver for mcp-camera.
//
//   node build_scripts/Makefile.mjs build [--debug] [--clean]
//   node build_scripts/Makefile.mjs test  [--filter=NAME] [--norun]
//   node build_scripts/Makefile.mjs fmt   [--check]
//   node build_scripts/Makefile.mjs run   [-- args...]
//   node build_scripts/Makefile.mjs clean
//
// Two unity builds, one translation unit each:
//   src/mcp.c     -> build/camera_mcp      (no GUI dependencies)
//   src/preview.c -> build/camera_preview  (sokol + X11 + GL + ALSA)

import {spawnSync} from 'node:child_process';
import {readdirSync, mkdirSync, rmSync, existsSync, writeFileSync} from 'node:fs';
import {join, dirname, basename} from 'node:path';
import {fileURLToPath} from 'node:url';

const ROOT = dirname(dirname(fileURLToPath(import.meta.url)));
const BUILD = join(ROOT, 'build');
const CC = process.env.CC || 'clang';

const argv = process.argv.slice(2);
const cmd = argv.find((a) => !a.startsWith('-')) || 'build';
const has = (f) => argv.includes(f);
const opt = (name, dflt = null) => {
  const hit = argv.find((a) => a.startsWith(`--${name}=`));
  return hit ? hit.slice(name.length + 3) : dflt;
};

const DEBUG = has('--debug');
// camera_preview links the window stack; camera_mcp deliberately does not, so
// the MCP server stays runnable on a headless box (it just cannot show a
// preview there).
const PREVIEW_LIBS = ['-lX11', '-lXi', '-lXcursor', '-lXrandr', '-lGL', '-lasound', '-lm',
                      '-ldl', '-lpthread'];
const MCP_LIBS = ['-lm', '-lpthread'];

function baseFlags() {
  const f = [`@${join(ROOT, 'clang_options.rsp')}`, '-I', join(ROOT, 'src')];
  if (DEBUG) {
    f.push('-O0', '-g', '-DDEBUG_SLOW', '-fno-omit-frame-pointer');
  } else {
    f.push('-O2');
  }
  return f;
}

function run(bin, args, opts = {}) {
  const r = spawnSync(bin, args, {cwd: ROOT, stdio: 'inherit', ...opts});
  if (r.error) throw r.error;
  return r.status ?? 1;
}

function compile(label, src, out, extraFlags, libs) {
  const t0 = Date.now();
  const args = [...baseFlags(), ...extraFlags, src, '-o', out, ...libs];
  process.stdout.write(`  ${label.padEnd(16)} `);
  const code = run(CC, args);
  if (0 !== code) {
    console.error(`\n  ${label}: FAILED`);
    return false;
  }
  console.log(`ok  (${Date.now() - t0} ms)`);
  return true;
}

function cmdClean() {
  rmSync(BUILD, {recursive: true, force: true});
  mkdirSync(BUILD, {recursive: true});
  writeFileSync(join(BUILD, '.gitkeep'), '');
  console.log('cleaned build/');
}

// clangd needs to know the two translation units; without this it analyses
// each .c on its own and reports a storm of phantom errors, because in a unity
// build no file compiles alone.
function writeCompileCommands() {
  const entry = (src, extra, libs) => ({
    directory: ROOT,
    arguments: [CC, ...baseFlags(), ...extra, src, '-o',
                join(BUILD, basename(src, '.c')), ...libs],
    file: join(ROOT, src),
  });
  const db = [
    entry('src/mcp.c', [], MCP_LIBS),
    entry('src/preview.c', ['-I', join(ROOT, 'vendor/sokol')], PREVIEW_LIBS),
    entry('test/ipc_client.c', [], MCP_LIBS),
  ];
  for (const f of readdirSync(join(ROOT, 'test/unit')).filter((f) => f.endsWith('.c'))) {
    db.push(entry(join('test/unit', f), [], MCP_LIBS));
  }
  writeFileSync(join(ROOT, 'compile_commands.json'), JSON.stringify(db, null, 2) + '\n');
}

function cmdBuild() {
  if (has('--clean')) cmdClean();
  mkdirSync(BUILD, {recursive: true});
  writeCompileCommands();
  console.log(`building (${DEBUG ? 'debug' : 'release'})`);
  const ok =
      compile('camera_mcp', 'src/mcp.c', join(BUILD, 'camera_mcp'), [], MCP_LIBS) &&
      compile('camera_preview', 'src/preview.c', join(BUILD, 'camera_preview'),
              ['-I', join(ROOT, 'vendor/sokol')], PREVIEW_LIBS) &&
      compile('ipc_client', 'test/ipc_client.c', join(BUILD, 'ipc_client'), [], MCP_LIBS);
  if (!ok) process.exit(1);
  console.log(`\nbinaries in ${BUILD}`);
  return 0;
}

// Each test under test/unit/ is its own executable, compiled against the same
// unity header, so a failing test is isolated to one process.
function cmdTest() {
  mkdirSync(BUILD, {recursive: true});
  const dir = join(ROOT, 'test/unit');
  const filter = opt('filter');
  const files = readdirSync(dir).filter((f) => f.endsWith('.c')).sort().filter(
      (f) => !filter || f.toLowerCase().includes(filter.toLowerCase()));
  if (0 === files.length) {
    console.error(`no tests matched${filter ? ` --filter=${filter}` : ''}`);
    return 1;
  }

  console.log('--tests--\n');
  let failures = 0, ran = 0;
  for (const [i, f] of files.entries()) {
    const name = basename(f, '.c');
    const out = join(BUILD, `test_${name}`);
    console.log(`${i + 1}) ${name}`);
    const t0 = Date.now();
    const code = run(CC, [...baseFlags(), join(dir, f), '-o', out, ...MCP_LIBS]);
    if (0 !== code) {
      console.log(`   compile FAILED\n`);
      failures++;
      continue;
    }
    console.log(`   compiled in ${Date.now() - t0} ms`);
    if (has('--norun')) continue;
    const rc = run(out, []);
    ran++;
    if (0 !== rc) {
      console.log(`   process FAILED. code: ${rc}\n`);
      failures++;
    } else {
      console.log(`   process succeeded. code: 0\n`);
    }
  }
  console.log(`${ran} run, ${failures} failures. (${files.length} found)`);
  return failures > 0 ? 1 : 0;
}

function cmdFmt() {
  const srcs = [];
  const walk = (d) => {
    for (const e of readdirSync(d, {withFileTypes: true})) {
      const p = join(d, e.name);
      if (e.isDirectory()) {
        if ('vendor' === e.name || 'node_modules' === e.name) continue;
        walk(p);
      } else if (e.name.endsWith('.c') || e.name.endsWith('.h')) {
        srcs.push(p);
      }
    }
  };
  walk(join(ROOT, 'src'));
  walk(join(ROOT, 'test'));
  const args = has('--check') ? ['--dry-run', '--Werror', ...srcs] : ['-i', ...srcs];
  return run('clang-format', args);
}

function cmdRun() {
  const bin = join(BUILD, 'camera_mcp');
  if (!existsSync(bin)) {
    console.error('build first: node build_scripts/Makefile.mjs build');
    return 1;
  }
  const sep = argv.indexOf('--');
  return run(bin, sep >= 0 ? argv.slice(sep + 1) : []);
}

const COMMANDS = {build: cmdBuild, test: cmdTest, fmt: cmdFmt, run: cmdRun, clean: cmdClean};
const fn = COMMANDS[cmd];
if (!fn) {
  console.error(`unknown command '${cmd}'. try: ${Object.keys(COMMANDS).join(', ')}`);
  process.exit(2);
}
process.exit(fn() || 0);
