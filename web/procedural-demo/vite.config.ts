import { defineConfig } from 'vite';

export default defineConfig({
  // Relative URLs also work when the static build is hosted in a subdirectory.
  base: './',
  server: { host: '127.0.0.1' },
  build: { target: 'es2022' },
});
