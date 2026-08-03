/** @type {import('next').NextConfig} */
const nextConfig = {
  reactStrictMode: true,
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
