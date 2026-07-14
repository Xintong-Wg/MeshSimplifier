export function configureCadUploadServer(server) {
  // Large CAD files can take much longer than Node's default five-minute request window over a LAN.
  server.requestTimeout = 0;
  server.timeout = 0;
  server.headersTimeout = 120_000;
  server.keepAliveTimeout = 65_000;
}
