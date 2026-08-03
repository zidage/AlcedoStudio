import React from "react";
import type { Metadata } from "next";
import { AppProviders } from "./providers";

export const metadata: Metadata = {
  title: "Alcedo UI Fuzz Dashboard",
  description: "Live run controls for the Alcedo Studio UI fuzz automation platform",
};

export default function RootLayout({ children }: { children: React.ReactNode }): React.ReactElement {
  return (
    <html lang="en">
      <body style={{ margin: 0, minHeight: "100vh", background: "#f5f5f5" }}>
        <AppProviders>{children}</AppProviders>
      </body>
    </html>
  );
}
