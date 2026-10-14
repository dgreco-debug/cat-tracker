import { defineConfig } from 'vite';
import preact from '@preact/preset-vite';

// Served from https://<user>.github.io/cat-tracker/ on GitHub Pages
export default defineConfig({
  base: process.env.BASE_PATH ?? '/cat-tracker/',
  plugins: [preact()],
  build: { outDir: 'dist', sourcemap: false },
});
