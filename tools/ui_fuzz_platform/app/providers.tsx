"use client";

//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

import React from "react";
import { AntdRegistry } from "@ant-design/nextjs-registry";
import { App, ConfigProvider, theme as antdTheme } from "antd";
import { QueryClient, QueryClientProvider } from "@tanstack/react-query";

const queryClient = new QueryClient({
  defaultOptions: {
    queries: {
      refetchOnWindowFocus: false,
      retry: 1,
    },
  },
});

/**
 * Ant Design Pro v6 stack wiring for the dashboard: antd 6 CSS variables
 * (`cssVar`), ProComponents consumers, and React Query (Pro v6 data layer).
 */
export function AppProviders({ children }: { children: React.ReactNode }): React.ReactElement {
  return (
    <AntdRegistry>
      <ConfigProvider
        theme={{
          cssVar: { key: "alcedo-ui-fuzz" },
          algorithm: antdTheme.defaultAlgorithm,
          token: {
            borderRadius: 6,
            colorPrimary: "#1677ff",
          },
        }}
      >
        <App>
          <QueryClientProvider client={queryClient}>{children}</QueryClientProvider>
        </App>
      </ConfigProvider>
    </AntdRegistry>
  );
}
