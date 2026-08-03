// Dev and production artifacts must not share a distDir: running `pnpm
// build:web` while the dev server is up corrupts `.next` (mixed dev/prod
// vendor chunks), which leaves pages rendered but unhydrated and dead.
const isProduction = process.env.NODE_ENV === "production";

/** @type {import('next').NextConfig} */
const nextConfig = {
  reactStrictMode: true,
  distDir: isProduction ? ".next" : ".next-dev",
  // The dashboard server owns /api and /ws; Next only renders UI pages.
  transpilePackages: ["@ant-design/pro-components", "@ant-design/icons", "antd"],
  webpack: (config) => {
    // The runner core (src/) uses NodeNext-style `.js` specifiers for its own
    // TypeScript files; teach webpack to resolve them to the .ts sources so
    // the editor page can share the flow-graph/scenario-parse modules.
    config.resolve.extensionAlias = {
      ...config.resolve.extensionAlias,
      ".js": [".ts", ".tsx", ".js"],
    };
    return config;
  },
};

export default nextConfig;
