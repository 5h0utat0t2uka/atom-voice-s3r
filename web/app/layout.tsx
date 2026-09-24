import type { Metadata } from "next";
import "./globals.css";

export const metadata: Metadata = {
  title: "Atom VoiceS3R",
  description: "Atom VoiceS3R の音声アシスタント",
};

export default function RootLayout({ children }: LayoutProps<"/">) {
  return (
    <html lang="ja">
      <body>{children}</body>
    </html>
  );
}
