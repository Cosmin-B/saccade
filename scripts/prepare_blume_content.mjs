import { createHash } from 'node:crypto';
import { existsSync } from 'node:fs';
import { mkdir, readFile, rm, writeFile } from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const repositoryRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const docsSiteRoot = path.join(repositoryRoot, 'docs-site');
const docsRoot = path.join(docsSiteRoot, 'docs');
const manifest = JSON.parse(await readFile(path.join(docsSiteRoot, 'source-manifest.json'), 'utf8'));
const pagesBySource = new Map(manifest.files.map((entry) => [entry.path, entry]));

if (manifest.files.length !== 15 || new Set(manifest.files.map((entry) => entry.route)).size !== manifest.files.length) {
  throw new Error('The Saccade source manifest must contain exactly 15 unique public routes.');
}

await rm(docsRoot, { recursive: true, force: true });
await mkdir(docsRoot, { recursive: true });
await mkdir(path.join(docsSiteRoot, 'public'), { recursive: true });

for (const entry of manifest.files) {
  const sourcePath = path.join(repositoryRoot, entry.path);
  const source = await readFile(sourcePath, 'utf8');
  const digest = createHash('sha256').update(source).digest('hex');
  if (digest !== entry.sha256) throw new Error(`Source changed from pinned ${manifest.revision}: ${entry.path}`);

  const title = getTitle(source, entry.path);
  const body = transformMarkdown(source, entry);
  const metadata = `---\ntitle: ${JSON.stringify(title)}\n${entry.route ? `slug: ${entry.route}\n` : ''}---\n\n`;
  const sourceUrl = `https://github.com/Cosmin-B/saccade/blob/${manifest.revision}/${githubPath(entry.path)}`;
  const editUrl = `https://github.com/Cosmin-B/saccade/edit/main/${githubPath(entry.path)}`;
  const output = `${metadata}${body.trimEnd()}\n\nSource: [View the pinned source](${sourceUrl}) · [Edit this source](${editUrl})\n`;
  const destination = path.join(docsRoot, entry.route ? `${entry.route}.md` : 'index.md');
  await mkdir(path.dirname(destination), { recursive: true });
  await writeFile(destination, output, 'utf8');
}

const icon = await readFile(path.join(repositoryRoot, 'assets', 'saccade-icon.png'));
await writeFile(path.join(docsSiteRoot, 'public', 'icon.png'), icon);
await writeFile(path.join(docsSiteRoot, 'public', 'apple-touch-icon.png'), icon);
await writeFile(path.join(docsSiteRoot, 'public', 'saccade-icon.png'), icon);

console.log(`Staged ${manifest.files.length} allowlisted Saccade pages for Blume ${manifest.revision}.`);

function getTitle(markdown, sourcePath) {
  const header = markdown.match(/^---\r?\n([\s\S]*?)\r?\n---\r?\n/);
  const frontmatterTitle = header?.[1].match(/^title:\s*(.*?)\s*$/m)?.[1];
  if (frontmatterTitle) return frontmatterTitle.replace(/^['"]|['"]$/g, '');
  let fenced = false;
  for (const line of markdown.split(/\r?\n/)) {
    if (/^\s{0,3}(`{3,}|~{3,})/.test(line)) fenced = !fenced;
    if (!fenced) {
      const heading = line.match(/^#\s+(.+?)\s*#*\s*$/);
      if (heading) return cleanTitle(heading[1]);
    }
  }
  return path.posix.basename(sourcePath, path.posix.extname(sourcePath)).replaceAll('-', ' ');
}

function cleanTitle(value) {
  return value.replace(/`/g, '').replace(/\[([^\]]+)\]\([^)]*\)/g, '$1').trim();
}

function transformMarkdown(markdown, currentEntry) {
  const output = [];
  let fence;
  let removedTitle = false;
  const title = getTitle(markdown, currentEntry.path);
  for (const line of markdown.split(/\r?\n/)) {
    const opener = line.match(/^\s{0,3}(`{3,}|~{3,})/);
    if (!fence && opener) {
      fence = opener[1];
      output.push(line);
      continue;
    }
    if (fence) {
      output.push(line);
      if (new RegExp(`^\\s{0,3}${fence[0]}{${fence.length},}\\s*$`).test(line)) fence = undefined;
      continue;
    }

    const heading = line.match(/^#\s+(.+?)\s*#*\s*$/);
    if (!removedTitle && heading && cleanTitle(heading[1]) === title) {
      removedTitle = true;
      continue;
    }
    output.push(rewriteReferences(line, currentEntry));
  }
  return output.join('\n').replace(/\n{3,}/g, '\n\n');
}

function rewriteReferences(line, currentEntry) {
  return line
    .replace(/(!?\[[^\]]*\]\()(<[^>]+>|[^)\s]+)([^)]*\))/g, (whole, prefix, targetToken, suffix) => {
      const wrapped = targetToken.startsWith('<') && targetToken.endsWith('>');
      const target = wrapped ? targetToken.slice(1, -1) : targetToken;
      const rewritten = rewriteTarget(target, currentEntry);
      return `${prefix}${wrapped ? `<${rewritten}>` : rewritten}${suffix}`;
    })
    .replace(/(\b(?:href|src)=['"])([^'"]+)(['"])/gi, (whole, prefix, target, quote) => {
      const pinnedIcon = `https://github.com/Cosmin-B/saccade/blob/${manifest.revision}/assets/saccade-icon.png`;
      const isLocalIcon = currentEntry.route === '' && target === 'assets/saccade-icon.png';
      const rewritten = /^src=/i.test(prefix) && (isLocalIcon || target === pinnedIcon) ? '/docs/saccade-icon.png' : rewriteTarget(target, currentEntry);
      return `${prefix}${rewritten}${quote}`;
    })
    .replace(/(^\s*\[[^\]]+\]:\s+)(<[^>]+>|\S+)(.*$)/, (whole, prefix, targetToken, suffix) => {
      const wrapped = targetToken.startsWith('<') && targetToken.endsWith('>');
      const target = wrapped ? targetToken.slice(1, -1) : targetToken;
      const rewritten = rewriteTarget(target, currentEntry);
      return `${prefix}${wrapped ? `<${rewritten}>` : rewritten}${suffix}`;
    });
}

function rewriteTarget(target, currentEntry) {
  if (!target || target.startsWith('#') || /^(?:https?:|mailto:|tel:|data:|javascript:|\/\/)/i.test(target)) return target;
  const splitAt = target.search(/[?#]/);
  const targetPath = splitAt === -1 ? target : target.slice(0, splitAt);
  const suffix = splitAt === -1 ? '' : target.slice(splitAt);
  if (!targetPath) return target;

  const sourceTarget = path.posix.normalize(path.posix.join(path.posix.dirname(currentEntry.path), decodeURIComponent(targetPath)));
  const doc = pagesBySource.get(sourceTarget);
  if (doc) return `${routeUrl(doc.route)}${suffix}`;
  if (sourceTarget === 'LICENSE') return `${githubUrl(sourceTarget)}${suffix}`;
  if (sourceTarget.startsWith('../') || sourceTarget.startsWith('/')) return target;

  if (existsSync(path.join(repositoryRoot, sourceTarget))) return `${githubUrl(sourceTarget)}${suffix}`;
  return target;
}

function routeUrl(route) {
  return route ? `/${route}` : '/';
}

function githubUrl(sourcePath) {
  return `https://github.com/Cosmin-B/saccade/blob/${manifest.revision}/${githubPath(sourcePath)}`;
}

function githubPath(value) {
  return value.split('/').map((part) => encodeURIComponent(part)).join('/');
}
