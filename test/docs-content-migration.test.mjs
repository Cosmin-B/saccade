import assert from 'node:assert/strict';
import { readFile, readdir } from 'node:fs/promises';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import test from 'node:test';
import { fileURLToPath } from 'node:url';

const repositoryRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const docsRoot = path.join(repositoryRoot, 'docs-site', 'docs');
const manifest = JSON.parse(await readFile(path.join(repositoryRoot, 'docs-site', 'source-manifest.json'), 'utf8'));

test('the Blume staging script publishes the README and only the 14 pinned public docs', async () => {
  const result = spawnSync(process.execPath, ['scripts/prepare_blume_content.mjs'], {
    cwd: repositoryRoot,
    encoding: 'utf8',
  });
  assert.equal(result.status, 0, `${result.stdout}\n${result.stderr}`);
  assert.equal(manifest.files.length, 15);
  const pages = await collectMarkdown(docsRoot);
  assert.equal(pages.length, manifest.files.length);

  for (const entry of manifest.files) {
    const target = path.join(docsRoot, entry.route ? `${entry.route}.md` : 'index.md');
    const output = await readFile(target, 'utf8');
    if (entry.route) assert.match(output, new RegExp(`^---\\ntitle: .+\\nslug: ${escapeRegExp(entry.route)}\\n---\\n`));
    else assert.match(output, /^---\ntitle: .+\n---\n/);
    assert.deepEqual(fencedPayloads(output), fencedPayloads(await readFile(path.join(repositoryRoot, entry.path), 'utf8')), entry.path);
  }

  const overview = await readFile(path.join(docsRoot, 'index.md'), 'utf8');
  assert.equal(/^# Saccade\s*$/m.test(overview), false);
  assert.match(overview, /\]\(\/product\/version-0\.1\)/);
  assert.match(overview, /<img src="\/docs\/saccade-icon\.png"/);
  assert.doesNotMatch(overview, /github\.com\/Cosmin-B\/saccade\/blob\/[^\s"]+assets\/saccade-icon\.png/);
  assert.deepEqual(
    await readFile(path.join(repositoryRoot, 'docs-site', 'public', 'saccade-icon.png')),
    await readFile(path.join(repositoryRoot, 'assets', 'saccade-icon.png')),
  );
  assert.match(overview, /https:\/\/github\.com\/Cosmin-B\/saccade\/blob\//);
  assert.equal(manifest.files.some((entry) => entry.path.includes('superpowers')), false);
  assert.equal(JSON.stringify(manifest).includes('/Users/cosmin'), false);
  const config = await readFile(path.join(repositoryRoot, 'docs-site', 'blume.config.ts'), 'utf8');
  for (const entry of manifest.files) {
    assert.ok(config.includes(`"/${entry.route}"`), `navigation is missing ${entry.route || '/'}`);
  }
  assert.match(config, /base:\s*["']\/docs["']/);
  assert.equal(/basePath\s*:/.test(config), false);
  assert.equal(await readFile(path.join(repositoryRoot, 'docs-site', 'public', 'icon.png')).then((value) => value.length > 0), true);
  assert.equal(await readFile(path.join(repositoryRoot, 'docs-site', 'public', 'apple-touch-icon.png')).then((value) => value.length > 0), true);
});

async function collectMarkdown(root) {
  const entries = await readdir(root, { withFileTypes: true });
  const files = [];
  for (const entry of entries) {
    const target = path.join(root, entry.name);
    if (entry.isDirectory()) files.push(...(await collectMarkdown(target)));
    if (entry.isFile() && entry.name.endsWith('.md')) files.push(target);
  }
  return files;
}

function fencedPayloads(markdown) {
  const payloads = [];
  let fence;
  let payload = [];
  for (const line of markdown.split(/\r?\n/)) {
    const opener = line.match(/^\s{0,3}(`{3,}|~{3,})/);
    if (!fence && opener) {
      fence = opener[1];
      payload = [];
    } else if (fence && new RegExp(`^\\s{0,3}${fence[0]}{${fence.length},}\\s*$`).test(line)) {
      payloads.push(payload.join('\n'));
      fence = undefined;
    } else if (fence) {
      payload.push(line);
    }
  }
  return payloads;
}

function escapeRegExp(value) {
  return value.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
}
