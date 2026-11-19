import { useState } from "react";
import { useTranslation } from "react-i18next";
import { BrandBar, Sidebar } from "@/components/sidebar";
import { UpdateNotice } from "@/components/updates";
import { useAppState, useControllerNav, useEvents, useLanguageSync, useNarrow } from "@/hooks";
import { cn, errorText } from "@/lib";
import { AboutPage, ExportPage, GamePage, LibraryPage, LogPage, ToolsPage } from "@/pages";
import { useUi } from "@/stores";

const PAGES = { library: LibraryPage, game: GamePage, export: ExportPage, tools: ToolsPage, log: LogPage, about: AboutPage };

export function App() {
  const { t } = useTranslation();
  useEvents();
  useLanguageSync();
  useControllerNav();
  const state = useAppState();
  const page = useUi((s) => s.page);
  const narrow = useNarrow();
  const menuHidden = useUi((s) => s.menuHidden) && !narrow; // upright, the rail stays
  const [scrolled, setScrolled] = useState(false);
  const Page = PAGES[page];
  return (
    <div className="relative isolate flex h-full">
      <div className="arcade-grid pointer-events-none absolute inset-0 -z-10" />
      <div className="pointer-events-none absolute inset-0 -z-10 bg-[radial-gradient(ellipse_at_top_right,rgba(255,179,71,0.08),transparent_50%),radial-gradient(ellipse_at_bottom_left,rgba(62,230,255,0.05),transparent_50%)]" />
      <Sidebar />
      <BrandBar />
      <main className={cn("relative flex flex-1 flex-col overflow-y-auto", !menuHidden && "pl-64 max-md:pl-16")} onScroll={(e) => setScrolled(e.currentTarget.scrollTop > 8)}>
        <div className="drag sticky top-0 z-10 h-9 shrink-0" data-tauri-drag-region>
          {/* full-screen mode: the page scrolled under the logo fades out before it */}
          <div className={cn("pointer-events-none absolute inset-x-0 top-0 h-28 bg-gradient-to-b from-background via-background/85 to-transparent opacity-0 transition-opacity duration-300", menuHidden && scrolled && "opacity-100")} />
        </div>
        {/* full-screen mode: the page gets the width, below the logo */}
        <div className={cn("mx-auto w-full px-10 pb-12 max-md:px-4", menuHidden ? "max-w-7xl pt-12" : "max-w-4xl")}>
          <UpdateNotice />
          {state.error ? <div className="text-destructive">{t("app.stateError", { error: errorText(state.error) })}</div> : <Page />}
        </div>
      </main>
    </div>
  );
}
