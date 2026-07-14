import assert from 'node:assert/strict';
import { createServer } from 'node:http';
import test from 'node:test';

import { configureCadUploadServer } from '../src/server-config.js';

test('CAD upload server disables Node request timeout for large uploads', () => {
  const server = createServer();

  configureCadUploadServer(server);

  assert.equal(server.requestTimeout, 0);
  assert.equal(server.timeout, 0);
  assert.ok(server.headersTimeout >= 120_000);
  assert.ok(server.keepAliveTimeout >= 60_000);
});
