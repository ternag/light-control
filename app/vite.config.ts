import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";

// In production the PWA is served by a node and calls /api on the same origin.
// In `vite dev`, proxy /api to a local server node (default :8080).
export default defineConfig({
  plugins: [react()],
  server: {
    proxy: {
      "/api": process.env.NODE_URL ?? "http://localhost:8080",
    },
  },
});
