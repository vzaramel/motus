#!/usr/bin/env node

import fs from 'node:fs/promises';
import { accessSync, constants as fsConstants } from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { createHost, instantiateRuntime } from '../src/host.js';

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);
const RUNTIME_WASM_DIR = path.resolve(__dirname, '..');
const REPO_ROOT = path.resolve(RUNTIME_WASM_DIR, '..');
const DEFAULT_WASM_PATH = path.join(RUNTIME_WASM_DIR, 'build', 'ygg-runtime.wasm');
const DEFAULT_YGG_BIN = path.join(REPO_ROOT, 'build', 'ygg');

function usage() {
  console.log(`Usage:
  node --inspect-brk runtime-wasm/scripts/step-debug.mjs [options]

Options:
  --source <file.ygg>            Compile YGG source to bytecode first.
  --bytecode <file.bc>           Use precompiled bytecode file.
  --wasm <file.wasm>             Runtime wasm file (default: runtime-wasm/build/ygg-runtime.wasm)
  --linked-manifest <file>       Manifest for linked imports when compiling source.
  --mode <page|component>        Render mode (default: component).
  --function <index>             Function index for component mode (default: 0).
  --props-json <json>            Component props JSON (default: null).
  --children-json <json>         Component children JSON (default: null).
  --trace                        Print VM step events to stdout.
  --no-pause                     Disable debugger pause on each VM step.
  --print-html                   Print rendered HTML to stdout.
  --help                         Show this help.

Notes:
  - Run with --inspect-brk to use Chrome DevTools step debugging.
  - Page bytecode with async data fetch may stop at YGG_AWAIT (-2) in this runner.
  - Component mode is recommended for reliable local step-through.
  - Source compiled via ygg CLI is not linker-patched for BC_COMPONENT_LINKED.
    For linked pages/components, pass prelinked bytecode from origin (/page/* or /component/*).
`);
}

function fail(msg) {
  console.error(`[step-debug] ${msg}`);
  process.exit(1);
}

function runChecked(cmd, args, cwd, label) {
  const result = spawnSync(cmd, args, { cwd, stdio: 'inherit' });
  if (result.status !== 0) {
    fail(`${label} failed: ${cmd} ${args.join(' ')}`);
  }
}

function fileExists(p) {
  try {
    accessSync(p, fsConstants.F_OK);
    return true;
  } catch {
    return false;
  }
}

function parseArgs(argv) {
  const out = {
    sourcePath: '',
    bytecodePath: '',
    wasmPath: DEFAULT_WASM_PATH,
    linkedManifestPath: '',
    mode: 'component',
    functionIndex: 0,
    propsJson: 'null',
    childrenJson: 'null',
    trace: false,
    pause: true,
    printHtml: false,
  };

  for (let i = 0; i < argv.length; i++) {
    const arg = argv[i];
    switch (arg) {
      case '--source':
        out.sourcePath = argv[++i] || '';
        break;
      case '--bytecode':
        out.bytecodePath = argv[++i] || '';
        break;
      case '--wasm':
        out.wasmPath = argv[++i] || '';
        break;
      case '--linked-manifest':
        out.linkedManifestPath = argv[++i] || '';
        break;
      case '--mode':
        out.mode = String(argv[++i] || '').toLowerCase();
        break;
      case '--function':
        out.functionIndex = Number(argv[++i] || 0);
        break;
      case '--props-json':
        out.propsJson = argv[++i] || 'null';
        break;
      case '--children-json':
        out.childrenJson = argv[++i] || 'null';
        break;
      case '--trace':
        out.trace = true;
        break;
      case '--no-pause':
        out.pause = false;
        break;
      case '--print-html':
        out.printHtml = true;
        break;
      case '--help':
      case '-h':
        usage();
        process.exit(0);
      default:
        fail(`Unknown argument: ${arg}`);
    }
  }

  if (!out.sourcePath && !out.bytecodePath) {
    fail('Provide --source or --bytecode');
  }
  if (out.sourcePath && out.bytecodePath) {
    fail('Use either --source or --bytecode, not both');
  }
  if (out.mode !== 'page' && out.mode !== 'component') {
    fail(`Invalid --mode: ${out.mode} (expected page|component)`);
  }
  if (!Number.isInteger(out.functionIndex) || out.functionIndex < 0 || out.functionIndex > 0xFFFF) {
    fail(`Invalid --function: ${out.functionIndex}`);
  }
  return out;
}

async function resolvePathInput(raw, label) {
  const fromCwd = path.resolve(process.cwd(), raw);
  if (fileExists(fromCwd)) return fromCwd;
  const fromRepoRoot = path.resolve(REPO_ROOT, raw);
  if (fileExists(fromRepoRoot)) return fromRepoRoot;
  fail(`${label} not found: ${raw}`);
}

async function ensureWasmBuilt(wasmPath) {
  if (fileExists(wasmPath)) return;
  console.log('[step-debug] runtime wasm not found; building debug runtime...');
  runChecked('./build.sh', ['wasm-debug'], RUNTIME_WASM_DIR, 'build wasm-debug');
  if (!fileExists(wasmPath)) {
    fail(`runtime wasm still not found after build: ${wasmPath}`);
  }
}

async function ensureYggBuilt() {
  if (fileExists(DEFAULT_YGG_BIN)) return;
  console.log('[step-debug] ygg CLI not found; building...');
  runChecked('make', ['cli'], REPO_ROOT, 'build cli');
  if (!fileExists(DEFAULT_YGG_BIN)) {
    fail(`ygg CLI not found after build: ${DEFAULT_YGG_BIN}`);
  }
}

async function compileSourceToBytecode(sourcePath, linkedManifestPath) {
  await ensureYggBuilt();
  const outPath = path.join(
    os.tmpdir(),
    `ygg-step-debug-${Date.now()}-${Math.random().toString(16).slice(2)}.bc`,
  );
  const args = [
    '--input', sourcePath,
    '--target', 'bytecode',
    '--bytecode-out', outPath,
  ];
  if (linkedManifestPath) {
    args.push('--linked-manifest', linkedManifestPath);
  }
  runChecked(DEFAULT_YGG_BIN, args, REPO_ROOT, 'compile source');
  return outPath;
}

function parseJsonArg(label, raw) {
  try {
    return JSON.parse(raw);
  } catch (err) {
    fail(`Invalid ${label} JSON: ${err?.message || String(err)}`);
  }
}

function opcodeName(op) {
  const names = {
    0: 'BC_NOP',
    1: 'BC_CONST',
    36: 'BC_EMIT_LITERAL',
    45: 'BC_FETCH_DATA',
    51: 'BC_COMPONENT_START',
    53: 'BC_COMPONENT_LOAD',
    64: 'BC_COMPONENT_LINKED',
  };
  return names[op] || `OP_${op}`;
}

async function main() {
  const opts = parseArgs(process.argv.slice(2));

  const wasmPath = await resolvePathInput(opts.wasmPath, 'wasm file');
  await ensureWasmBuilt(wasmPath);

  let bytecodePath = opts.bytecodePath;
  let resolvedSourcePath = '';
  if (opts.sourcePath) {
    const sourcePath = await resolvePathInput(opts.sourcePath, 'source file');
    resolvedSourcePath = sourcePath;
    const manifestPath = opts.linkedManifestPath
      ? await resolvePathInput(opts.linkedManifestPath, 'linked manifest')
      : '';
    bytecodePath = await compileSourceToBytecode(sourcePath, manifestPath);
    console.log(`[step-debug] Compiled bytecode: ${bytecodePath}`);
  } else {
    bytecodePath = await resolvePathInput(opts.bytecodePath, 'bytecode file');
  }

  const props = parseJsonArg('props', opts.propsJson);
  const children = parseJsonArg('children', opts.childrenJson);

  if (!process.execArgv.some((arg) => arg.startsWith('--inspect'))) {
    console.warn('[step-debug] Tip: run with --inspect-brk to attach Chrome DevTools.');
  }

  const wasmBytes = new Uint8Array(await fs.readFile(wasmPath));
  const bytecode = new Uint8Array(await fs.readFile(bytecodePath));
  let outputText = '';
  let stepCount = 0;

  const writer = {
    async write(chunk) {
      const bytes = chunk instanceof Uint8Array ? chunk : new Uint8Array(chunk);
      outputText += new TextDecoder().decode(bytes);
    },
  };

  const host = createHost({
    writer,
    encoder: new TextEncoder(),
    fetchData: async () => null,
    loadComponent: async () => '',
    defaultSourcePath: resolvedSourcePath,
    onDebugStep: (event) => {
      stepCount += 1;
      if (!opts.trace) return;
      const source = event.sourcePath ? `${event.sourcePath}:${event.line}` : `pc=${event.pc}`;
      console.log(
        `[step] #${stepCount} ${source} ${opcodeName(event.opcode)} ` +
        `(chunk=${event.chunkKind}:${event.chunkIndex} pc=${event.pc})`,
      );
    },
    pauseOnDebugStep: opts.pause,
  });

  const runtime = await instantiateRuntime(wasmBytes, host);
  const initRc = runtime.init(bytecode);
  if (initRc !== 0) {
    fail(`ygg_init failed (${initRc})`);
  }

  runtime.setDebugStepMode(true);
  runtime.setPauseOnDebugStep(opts.pause);

  let rc = -1;
  if (opts.mode === 'component') {
    rc = runtime.renderComponent(opts.functionIndex, props, children);
  } else {
    rc = runtime.render();
  }

  if (rc === -2) {
    console.error('[step-debug] Render paused with YGG_AWAIT (-2). This runner does not auto-resume async fetches.');
  } else if (rc !== 0) {
    fail(`render failed (${rc})`);
  }

  await host.flush();
  runtime.free();

  console.log(`[step-debug] Render finished with rc=${rc}, steps=${stepCount}, htmlBytes=${Buffer.byteLength(outputText)}`);
  if (opts.printHtml) {
    process.stdout.write(`${outputText}\n`);
  }
}

main().catch((err) => {
  fail(err?.stack || err?.message || String(err));
});
