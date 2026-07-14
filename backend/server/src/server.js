import { createServer } from 'node:http';
import { spawn } from 'node:child_process';
import { appendFile, mkdir, readFile, rm, stat, writeFile } from 'node:fs/promises';
import { createReadStream, createWriteStream } from 'node:fs';
import { pipeline } from 'node:stream/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import crypto from 'node:crypto';
import { configureCadUploadServer } from './server-config.js';

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);
const rootDir = path.resolve(__dirname, '..', '..', '..');
const dataDir = path.join(rootDir, 'data');
const outputDir = path.join(dataDir, 'outputs');
const importDir = path.join(dataDir, 'imports');
const inspectDir = path.join(dataDir, 'inspections');
const cacheDir = path.join(dataDir, 'cache');
const logDir = path.join(dataDir, 'logs');
const tempDir = path.join(dataDir, 'tmp');
const webDistDir = path.join(rootDir, 'web', 'dist');

function defaultCliPath() {
  const exeName = process.platform === 'win32' ? 'MeshSimplifierCli.exe' : 'MeshSimplifierCli';
  const candidates = process.platform === 'win32'
    ? [
        path.join(rootDir, 'build', 'bin', 'Release', exeName),
        path.join(rootDir, 'build', 'bin', exeName),
      ]
    : [
        path.join(rootDir, 'build', 'bin', exeName),
      ];
  return candidates[0];
}

const cliPath = process.env.MESH_SIMPLIFIER_CLI || defaultCliPath();
const port = Number(process.env.MESH_SIMPLIFIER_PORT || 8877);

function sendJson(res, status, body) {
  const payload = JSON.stringify(body);
  res.writeHead(status, {
    'content-type': 'application/json; charset=utf-8',
    'content-length': Buffer.byteLength(payload),
    'access-control-allow-origin': '*',
    'access-control-allow-methods': 'GET,POST,OPTIONS',
    'access-control-allow-headers': 'content-type',
  });
  res.end(payload);
}

function sendError(res, status, message, details = {}) {
  if (res.destroyed || res.writableEnded) return;
  sendJson(res, status, { ok: false, error: message, ...details });
}

function errorText(error) {
  return error instanceof Error ? error.message : String(error);
}

async function appendImportLog(logPath, message) {
  await mkdir(logDir, { recursive: true });
  await appendFile(logPath, `[${new Date().toISOString()}] ${message}\n`, 'utf8');
}

class ImportError extends Error {
  constructor(stage, message) {
    super(message);
    this.stage = stage;
  }
}

function contentTypeFor(filePath) {
  const extension = path.extname(filePath).toLowerCase();
  switch (extension) {
    case '.html':
      return 'text/html; charset=utf-8';
    case '.js':
      return 'text/javascript; charset=utf-8';
    case '.css':
      return 'text/css; charset=utf-8';
    case '.json':
      return 'application/json; charset=utf-8';
    case '.png':
      return 'image/png';
    case '.jpg':
    case '.jpeg':
      return 'image/jpeg';
    case '.svg':
      return 'image/svg+xml';
    case '.ico':
      return 'image/x-icon';
    case '.wasm':
      return 'application/wasm';
    default:
      return 'application/octet-stream';
  }
}

async function readJson(req) {
  const chunks = [];
  for await (const chunk of req) chunks.push(chunk);
  if (!chunks.length) return {};
  return JSON.parse(Buffer.concat(chunks).toString('utf8'));
}

function safeImportName(name) {
  const decoded = decodeURIComponent(String(name || 'model.step'));
  return decoded.replace(/[<>:"/\\|?*\x00-\x1F]/g, '_').slice(0, 180) || 'model.step';
}

function safeExportName(name, fallback = 'export') {
  const normalized = String(name || fallback)
    .replace(/[<>:"/\\|?*\x00-\x1F]/g, '_')
    .replace(/\s+/g, ' ')
    .replace(/[. ]+$/g, '')
    .trim();
  return normalized.slice(0, 180) || fallback;
}

function isCadFileName(name) {
  return /\.(step|stp|iges|igs)$/i.test(name);
}

function safeCadExtension(name) {
  const match = String(name || '').match(/\.(step|stp|iges|igs)$/i);
  return match ? `.${match[1].toLowerCase()}` : '.step';
}

function normalizeExportFormat(value) {
  const format = String(value || 'glb').toLowerCase();
  if (format !== 'glb' && format !== 'stl') return 'glb';
  return format;
}

function normalizeNodeIds(value) {
  if (!value) return [];
  const rawItems = Array.isArray(value) ? value : [value];
  return [...new Set(rawItems
    .map((item) => String(item || '').trim())
    .filter(Boolean)
    .filter((item) => !/^name:/i.test(item) && !/^shape:/i.test(item))
    .map((item) => item.slice(0, 512)))];
}

function normalizeExcludedNodeIds(value) {
  return normalizeNodeIds(value);
}

function normalizeIncludedNodeIds(value) {
  return normalizeNodeIds(value);
}

async function appendNodeArgs(args, nodeIds, jobId, suffix, itemFlag, fileFlag) {
  if (nodeIds.length > 64) {
    await mkdir(tempDir, { recursive: true });
    const nodePath = path.join(tempDir, `${jobId}-${suffix}.txt`);
    await writeFile(nodePath, `${nodeIds.join('\n')}\n`, 'utf8');
    args.push(fileFlag, nodePath);
  } else {
    for (const id of nodeIds) {
      args.push(itemFlag, id);
    }
  }
}

async function appendExcludedNodeArgs(args, body, jobId) {
  const excludedNodeIds = normalizeExcludedNodeIds(body.excludedNodeIds || body.deletedNodeIds);
  await appendNodeArgs(args, excludedNodeIds, jobId, 'excluded-nodes', '--exclude-node', '--exclude-nodes-file');
  return excludedNodeIds;
}

async function appendIncludedNodeArgs(args, body, jobId) {
  const includeNodeIds = normalizeIncludedNodeIds(body.includeNodeIds || body.includedNodeIds);
  await appendNodeArgs(args, includeNodeIds, jobId, 'include-nodes', '--include-node', '--include-nodes-file');
  return includeNodeIds;
}

function outputUrlFor(filePath) {
  const relative = path.relative(outputDir, filePath).split(path.sep).map(encodeURIComponent).join('/');
  return `/outputs/${relative}`;
}

const crc32Table = (() => {
  const table = new Uint32Array(256);
  for (let i = 0; i < 256; i += 1) {
    let value = i;
    for (let bit = 0; bit < 8; bit += 1) {
      value = (value & 1) ? (0xedb88320 ^ (value >>> 1)) : (value >>> 1);
    }
    table[i] = value >>> 0;
  }
  return table;
})();

function updateCrc32(chunk, previous = 0) {
  let crc = previous ^ 0xffffffff;
  for (const byte of chunk) {
    crc = crc32Table[(crc ^ byte) & 0xff] ^ (crc >>> 8);
  }
  return (crc ^ 0xffffffff) >>> 0;
}

function dosDateTime(date = new Date()) {
  const year = Math.max(1980, date.getFullYear());
  return {
    time: (date.getHours() << 11) | (date.getMinutes() << 5) | Math.floor(date.getSeconds() / 2),
    date: ((year - 1980) << 9) | ((date.getMonth() + 1) << 5) | date.getDate(),
  };
}

function writeChunk(stream, chunk) {
  return new Promise((resolve, reject) => {
    const onError = (error) => {
      stream.off('drain', onDrain);
      reject(error);
    };
    const onDrain = () => {
      stream.off('error', onError);
      resolve();
    };
    stream.once('error', onError);
    if (stream.write(chunk)) {
      stream.off('error', onError);
      resolve();
    } else {
      stream.once('drain', onDrain);
    }
  });
}

function localZipHeader(fileName, modifiedAt) {
  const encodedName = Buffer.from(fileName, 'utf8');
  const stamp = dosDateTime(modifiedAt);
  const header = Buffer.alloc(30);
  header.writeUInt32LE(0x04034b50, 0);
  header.writeUInt16LE(20, 4);
  header.writeUInt16LE(0x0808, 6);
  header.writeUInt16LE(0, 8);
  header.writeUInt16LE(stamp.time, 10);
  header.writeUInt16LE(stamp.date, 12);
  header.writeUInt32LE(0, 14);
  header.writeUInt32LE(0, 18);
  header.writeUInt32LE(0, 22);
  header.writeUInt16LE(encodedName.length, 26);
  header.writeUInt16LE(0, 28);
  return Buffer.concat([header, encodedName]);
}

function zipDataDescriptor(crc, size) {
  const descriptor = Buffer.alloc(16);
  descriptor.writeUInt32LE(0x08074b50, 0);
  descriptor.writeUInt32LE(crc >>> 0, 4);
  descriptor.writeUInt32LE(size, 8);
  descriptor.writeUInt32LE(size, 12);
  return descriptor;
}

function centralZipHeader(entry) {
  const encodedName = Buffer.from(entry.name, 'utf8');
  const stamp = dosDateTime(entry.modifiedAt);
  const header = Buffer.alloc(46);
  header.writeUInt32LE(0x02014b50, 0);
  header.writeUInt16LE(20, 4);
  header.writeUInt16LE(20, 6);
  header.writeUInt16LE(0x0808, 8);
  header.writeUInt16LE(0, 10);
  header.writeUInt16LE(stamp.time, 12);
  header.writeUInt16LE(stamp.date, 14);
  header.writeUInt32LE(entry.crc >>> 0, 16);
  header.writeUInt32LE(entry.size, 20);
  header.writeUInt32LE(entry.size, 24);
  header.writeUInt16LE(encodedName.length, 28);
  header.writeUInt16LE(0, 30);
  header.writeUInt16LE(0, 32);
  header.writeUInt16LE(0, 34);
  header.writeUInt16LE(0, 36);
  header.writeUInt32LE(0, 38);
  header.writeUInt32LE(entry.offset, 42);
  return Buffer.concat([header, encodedName]);
}

function endOfCentralDirectory(entryCount, centralSize, centralOffset) {
  const footer = Buffer.alloc(22);
  footer.writeUInt32LE(0x06054b50, 0);
  footer.writeUInt16LE(0, 4);
  footer.writeUInt16LE(0, 6);
  footer.writeUInt16LE(entryCount, 8);
  footer.writeUInt16LE(entryCount, 10);
  footer.writeUInt32LE(centralSize, 12);
  footer.writeUInt32LE(centralOffset, 16);
  footer.writeUInt16LE(0, 20);
  return footer;
}

async function createStoredZip(files, zipPath, folderName) {
  await mkdir(path.dirname(zipPath), { recursive: true });
  const stream = createWriteStream(zipPath, { flags: 'w' });
  const entries = [];
  let offset = 0;

  try {
    for (const file of files) {
      const sourcePath = String(file.path || '');
      const info = await stat(sourcePath);
      if (!info.isFile()) continue;
      if (info.size > 0xffffffff) throw new Error(`文件过大，暂不支持 ZIP64：${sourcePath}`);

      const safeFileName = safeExportName(file.fileName || file.name || path.basename(sourcePath), 'part.stl');
      const entryName = `${safeExportName(folderName, 'parts')}/${safeFileName.toLowerCase().endsWith('.stl') ? safeFileName : `${safeFileName}.stl`}`.replace(/\\/g, '/');
      const header = localZipHeader(entryName, info.mtime);
      const entryOffset = offset;
      await writeChunk(stream, header);
      offset += header.length;

      let crc = 0;
      let bytes = 0;
      for await (const chunk of createReadStream(sourcePath)) {
        crc = updateCrc32(chunk, crc);
        bytes += chunk.length;
        await writeChunk(stream, chunk);
        offset += chunk.length;
      }
      if (bytes !== info.size) throw new Error(`ZIP 写入失败：${sourcePath}`);

      const descriptor = zipDataDescriptor(crc, bytes);
      await writeChunk(stream, descriptor);
      offset += descriptor.length;

      entries.push({
        name: entryName,
        modifiedAt: info.mtime,
        crc,
        size: bytes,
        offset: entryOffset,
      });
    }

    if (!entries.length) throw new Error('没有可写入 ZIP 的 STL 文件');

    const centralOffset = offset;
    for (const entry of entries) {
      const centralHeader = centralZipHeader(entry);
      await writeChunk(stream, centralHeader);
      offset += centralHeader.length;
    }
    const centralSize = offset - centralOffset;
    const footer = endOfCentralDirectory(entries.length, centralSize, centralOffset);
    await writeChunk(stream, footer);
    offset += footer.length;

    await new Promise((resolve, reject) => {
      stream.end(resolve);
      stream.once('error', reject);
    });
  } catch (error) {
    stream.destroy();
    throw error;
  }

  return { entryCount: entries.length, size: offset };
}

function runCli(args, label = '') {
  return new Promise((resolve, reject) => {
    const child = spawn(cliPath, args, {
      cwd: rootDir,
      windowsHide: true,
      stdio: ['ignore', 'pipe', 'pipe'],
    });
    let stdout = '';
    let stderr = '';
    child.stdout.on('data', (chunk) => { stdout += chunk.toString(); });
    child.stderr.on('data', (chunk) => { stderr += chunk.toString(); });
    child.on('error', reject);
    child.on('close', (code) => {
      if (code === 0) resolve({ stdout, stderr });
      else reject(new Error(`${label ? `${label}: ` : ''}${stderr || stdout || `MeshSimplifierCli exited with ${code}`}`));
    });
  });
}

async function handleConvert(req, res) {
  const body = await readJson(req);
  const inputPath = String(body.inputPath || '').trim();
  if (!inputPath) {
    sendError(res, 400, 'inputPath is required. Use an absolute Windows path to a STEP, STP, IGES, or IGS file.');
    return;
  }

  await mkdir(outputDir, { recursive: true });
  const jobId = `${Date.now()}-${crypto.randomBytes(4).toString('hex')}`;
  const format = normalizeExportFormat(body.format);
  const outputPath = path.join(outputDir, `${jobId}.${format}`);
  const metadataPath = path.join(outputDir, `${jobId}.json`);

  const args = [
    '--input', inputPath,
    '--output', outputPath,
    '--format', format,
    '--metadata', metadataPath,
    '--ratio', String(body.ratio ?? 0.35),
    '--error', String(body.error ?? 0.05),
    '--linear-deflection', String(body.linearDeflection ?? 0.5),
    '--angular-deflection', String(body.angularDeflection ?? 0.5),
  ];
  if (body.cacheDir) args.push('--cache-dir', String(body.cacheDir));
  if (body.targetMB) args.push('--target-mb', String(body.targetMB));
  if (body.draco) args.push('--draco');
  const includeNodeIds = await appendIncludedNodeArgs(args, body, jobId);
  const excludedNodeIds = await appendExcludedNodeArgs(args, body, jobId);

  const result = await runCli(args);
  let metadata = {};
  try {
    metadata = JSON.parse(await readFile(metadataPath, 'utf8'));
  } catch {
    metadata = {};
  }

  sendJson(res, 200, {
    ok: true,
    jobId,
    format,
    modelUrl: format === 'glb' ? `/outputs/${jobId}.glb` : undefined,
    fileUrl: `/outputs/${jobId}.${format}`,
    metadataUrl: `/outputs/${jobId}.json`,
    metadata,
    excludedNodeIds,
    includeNodeIds,
    cli: result.stdout.trim(),
  });
}

async function handleBatchExport(req, res) {
  const body = await readJson(req);
  const inputPath = String(body.inputPath || '').trim();
  if (!inputPath) {
    sendError(res, 400, 'inputPath is required. Import a STEP, STP, IGES, or IGS file first.');
    return;
  }

  await mkdir(outputDir, { recursive: true });
  const jobId = `${Date.now()}-${crypto.randomBytes(4).toString('hex')}`;
  const partsDir = path.join(outputDir, `${jobId}-parts`);
  const metadataPath = path.join(outputDir, `${jobId}-parts.json`);
  const folderName = safeExportName(body.folderName, `${jobId}-parts`);

  const args = [
    '--input', inputPath,
    '--batch-parts',
    '--output-dir', partsDir,
    '--metadata', metadataPath,
    '--ratio', String(body.ratio ?? 1),
    '--error', String(body.error ?? 0.05),
    '--linear-deflection', String(body.linearDeflection ?? 0.5),
    '--angular-deflection', String(body.angularDeflection ?? 0.5),
  ];
  if (body.cacheDir) args.push('--cache-dir', String(body.cacheDir));
  if (body.targetMB) args.push('--target-mb', String(body.targetMB));
  const includeNodeIds = await appendIncludedNodeArgs(args, body, jobId);
  const excludedNodeIds = await appendExcludedNodeArgs(args, body, jobId);

  const result = await runCli(args);
  let metadata = {};
  try {
    metadata = JSON.parse(await readFile(metadataPath, 'utf8'));
  } catch {
    metadata = {};
  }

  const files = Array.isArray(metadata.files)
    ? metadata.files.map((file) => ({
        ...file,
        url: outputUrlFor(String(file.path || '')),
      }))
    : [];
  const zipPath = path.join(outputDir, `${jobId}-${folderName}.zip`);
  const zip = await createStoredZip(files, zipPath, folderName);

  sendJson(res, 200, {
    ok: true,
    jobId,
    format: 'stl-parts',
    folderPath: partsDir,
    zipUrl: outputUrlFor(zipPath),
    zipFileName: path.basename(zipPath),
    zip,
    metadataUrl: `/outputs/${jobId}-parts.json`,
    metadata,
    files,
    excludedNodeIds,
    includeNodeIds,
    cli: result.stdout.trim(),
  });
}

async function handleImport(req, res) {
  const originalName = safeImportName(req.headers['x-file-name']);
  if (!isCadFileName(originalName)) {
    sendError(res, 400, '请选择 STEP, STP, IGES 或 IGS 模型文件。');
    return;
  }

  await mkdir(importDir, { recursive: true });
  await mkdir(inspectDir, { recursive: true });
  await mkdir(cacheDir, { recursive: true });
  await mkdir(outputDir, { recursive: true });
  const jobId = `${Date.now()}-${crypto.randomBytes(4).toString('hex')}`;
  const importPath = path.join(importDir, `${jobId}${safeCadExtension(originalName)}`);
  const jobCacheDir = path.join(cacheDir, jobId);
  const logPath = path.join(logDir, `${jobId}.log`);
  let bytesWritten = 0;

  try {
    await appendImportLog(logPath, `导入开始：${originalName}，声明大小 ${req.headers['content-length'] || '未知'} 字节。`);
    req.on('data', (chunk) => {
      bytesWritten += chunk.length;
    });
    req.once('aborted', () => {
      void appendImportLog(logPath, `上传连接被客户端中止，已接收 ${bytesWritten} 字节。`);
    });
    try {
      await pipeline(req, createWriteStream(importPath, { flags: 'wx' }));
    } catch (error) {
      await appendImportLog(logPath, `上传失败：${errorText(error)}`);
      await rm(importPath, { force: true });
      throw new ImportError('upload', `上传模型失败：${errorText(error)}`);
    }

    if (!bytesWritten) throw new ImportError('upload', '导入文件为空。');
    await appendImportLog(logPath, `上传完成：${bytesWritten} 字节，开始解析装配层级。`);

    const inspectionPath = path.join(inspectDir, `${jobId}.json`);
    let inspection = {};
    try {
      const result = await runCli([
        '--inspect',
        '--input', importPath,
        '--cache-dir', jobCacheDir,
        '--metadata', inspectionPath,
      ], 'inspect');
      await appendImportLog(logPath, `装配层级解析完成。${result.stdout.trim()}`);
      inspection = JSON.parse(await readFile(inspectionPath, 'utf8'));
    } catch (error) {
      await appendImportLog(logPath, `装配层级解析失败：${errorText(error)}`);
      throw new ImportError('inspect', `导入完成，但装配层级解析失败：${errorText(error)}`);
    }

    const previewOutputPath = path.join(outputDir, `${jobId}-original.glb`);
    const previewMetadataPath = path.join(outputDir, `${jobId}-original.json`);
    let previewMetadata = inspection;
    let previewCli = '';
    try {
      await appendImportLog(logPath, '装配层级解析完成，开始三角化并生成原模型预览。');
      const result = await runCli([
        '--input', importPath,
        '--output', previewOutputPath,
        '--metadata', previewMetadataPath,
        '--cache-dir', jobCacheDir,
        '--ratio', '1',
        '--error', '0.05',
        '--linear-deflection', '0.5',
        '--angular-deflection', '0.5',
      ], 'preview');
      previewCli = result.stdout.trim();
      await appendImportLog(logPath, `原模型预览完成。${previewCli}`);
      const parsedPreview = JSON.parse(await readFile(previewMetadataPath, 'utf8'));
      previewMetadata = {
        ...inspection,
        ...parsedPreview,
        hierarchy: parsedPreview.hierarchy || inspection.hierarchy,
        partCount: parsedPreview.partCount ?? inspection.partCount,
        shapeCount: parsedPreview.shapeCount ?? inspection.shapeCount,
      };
    } catch (error) {
      await appendImportLog(logPath, `原模型预览失败：${errorText(error)}`);
      throw new ImportError('preview', `导入完成，但原模型预览生成失败：${errorText(error)}`);
    }

    await appendImportLog(logPath, '导入成功。');
    sendJson(res, 200, {
      ok: true,
      jobId,
      fileName: originalName,
      inputPath: importPath,
      size: bytesWritten,
      cacheDir: jobCacheDir,
      modelUrl: `/outputs/${jobId}-original.glb`,
      metadataUrl: `/outputs/${jobId}-original.json`,
      metadata: previewMetadata,
      inspection,
      cli: previewCli,
    });
  } catch (error) {
    const stage = error instanceof ImportError ? error.stage : 'server';
    const message = errorText(error);
    await appendImportLog(logPath, `导入终止 [${stage}]：${message}`);
    sendError(res, stage === 'upload' ? 400 : 500, message, {
      stage,
      logFile: path.relative(rootDir, logPath),
    });
  }
}

async function serveOutput(req, res) {
  const url = new URL(req.url, `http://localhost:${port}`);
  const decodedPath = decodeURIComponent(url.pathname.replace(/^\/outputs\/?/, ''));
  const normalizedPath = path.normalize(decodedPath).replace(/^(\.\.[/\\])+/, '');
  const filePath = path.join(outputDir, normalizedPath);
  const resolved = path.resolve(filePath);
  if (!resolved.startsWith(path.resolve(outputDir))) {
    sendError(res, 400, 'Invalid output path');
    return;
  }
  try {
    const info = await stat(filePath);
    const isJson = filePath.endsWith('.json');
    const isStl = filePath.endsWith('.stl');
    const isZip = filePath.endsWith('.zip');
    const downloadName = encodeURIComponent(path.basename(filePath));
    res.writeHead(200, {
      'content-type': isJson ? 'application/json; charset=utf-8' : (isZip ? 'application/zip' : (isStl ? 'model/stl' : 'model/gltf-binary')),
      'content-length': info.size,
      'content-disposition': `attachment; filename*=UTF-8''${downloadName}`,
      'access-control-allow-origin': '*',
    });
    createReadStream(filePath).pipe(res);
  } catch {
    sendError(res, 404, 'Output not found');
  }
}

async function serveStatic(req, res) {
  const url = new URL(req.url, `http://localhost:${port}`);
  const decodedPath = decodeURIComponent(url.pathname);
  const requestedPath = decodedPath === '/' ? '/index.html' : decodedPath;
  const normalizedPath = path.normalize(requestedPath).replace(/^([/\\])+/, '');
  let filePath = path.join(webDistDir, normalizedPath);
  let resolved = path.resolve(filePath);
  const resolvedDist = path.resolve(webDistDir);

  if (!resolved.startsWith(resolvedDist)) {
    sendError(res, 400, 'Invalid static path');
    return;
  }

  try {
    let info = await stat(filePath);
    if (!info.isFile()) throw new Error('not a file');
  } catch {
    filePath = path.join(webDistDir, 'index.html');
    resolved = path.resolve(filePath);
    if (!resolved.startsWith(resolvedDist)) {
      sendError(res, 400, 'Invalid static path');
      return;
    }
  }

  try {
    const info = await stat(filePath);
    res.writeHead(200, {
      'content-type': contentTypeFor(filePath),
      'content-length': info.size,
      'cache-control': path.basename(filePath) === 'index.html' ? 'no-store' : 'public, max-age=31536000, immutable',
    });
    createReadStream(filePath).pipe(res);
  } catch {
    res.writeHead(503, { 'content-type': 'text/html; charset=utf-8' });
    res.end('<!doctype html><meta charset="utf-8"><title>Mesh Simplifier</title><body><h1>前端页面未构建</h1><p>请先运行 <code>npm run build --prefix web</code>，再重新启动 MeshSimplifierServer.exe。</p></body>');
  }
}

const server = createServer(async (req, res) => {
  try {
    if (req.method === 'OPTIONS') {
      sendJson(res, 200, { ok: true });
      return;
    }
    const url = new URL(req.url, `http://localhost:${port}`);
    if (req.method === 'GET' && url.pathname === '/api/health') {
      sendJson(res, 200, { ok: true, cliPath, outputDir, importDir, inspectDir, cacheDir });
      return;
    }
    if (req.method === 'POST' && url.pathname === '/api/import') {
      await handleImport(req, res);
      return;
    }
    if (req.method === 'POST' && url.pathname === '/api/convert') {
      await handleConvert(req, res);
      return;
    }
    if (req.method === 'POST' && url.pathname === '/api/export-parts') {
      await handleBatchExport(req, res);
      return;
    }
    if (req.method === 'GET' && url.pathname.startsWith('/outputs/')) {
      await serveOutput(req, res);
      return;
    }
    if (req.method === 'GET' || req.method === 'HEAD') {
      await serveStatic(req, res);
      return;
    }
    sendError(res, 404, 'Not found');
  } catch (error) {
    sendError(res, 500, error instanceof Error ? error.message : String(error));
  }
});

configureCadUploadServer(server);

server.listen(port, () => {
  console.log(`Mesh Simplifier server listening on http://localhost:${port}`);
  console.log(`Open UI: http://127.0.0.1:${port}/`);
  console.log(`Using native CLI: ${cliPath}`);
});
