import { defineConfig } from "vite";
import solid from "vite-plugin-solid";

// https://vitejs.dev/config/
export default defineConfig({
  plugins: [solid()],

  // Vite options for Tauri
  clearScreen: false,
  server: {
    port: 5173,
    strictPort: true,
    host: '0.0.0.0', // Listen on all interfaces for Tauri webview
    watch: {
      // Tauri CLI uses this to determine when dev server is ready
      ignored: ["**/src-tauri/**"],
    },
  },
});
