/** @type {import('next').NextConfig} */
const nextConfig = {
  reactStrictMode: true,
  // The dashboard server owns /api and /ws; Next only renders UI pages.
  transpilePackages: ["@ant-design/pro-components", "@ant-design/icons", "antd"],
};

export default nextConfig;
