import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';

const apiPort = process.env.MESH_SIMPLIFIER_PORT || '8877';
const apiTarget = `http://127.0.0.1:${apiPort}`;

export default defineConfig({
  plugins: [react()],
  build: {
    chunkSizeWarningLimit: 1400,
  },
  server: {
    proxy: {
      '/api': apiTarget,
      '/outputs': apiTarget,
    },
  },
});
