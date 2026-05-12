import { defineConfig } from "vite";
import solid from "vite-plugin-solid";

// https://vitejs.dev/config/
export default defineConfig({
  plugins: [solid()],

  // Emit relative asset paths so the built bundle works under file:// (the
  // WKWebView loads index.html via loadFileURL). Default "/" makes asset
  // hrefs resolve to the filesystem root and the page comes up blank.
  base: "./",

  clearScreen: false,
  server: {
    port: 5173,
    strictPort: true,
  },
});
