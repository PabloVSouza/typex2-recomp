import { useTranslation } from "react-i18next";
import { Sidebar } from "@/components/sidebar";
import { useAppState, useEvents, useLanguageSync } from "@/hooks";
import { errorText } from "@/lib";
import { AboutPage, ExportPage, GamePage, LibraryPage, LogPage, ToolsPage } from "@/pages";
import { useUi } from "@/stores";

const PAGES = { library: LibraryPage, game: GamePage, export: ExportPage, tools: ToolsPage, log: LogPage, about: AboutPage };

export function App() {
  const { t } = useTranslation();
  useEvents();
  useLanguageSync();
  const state = useAppState();
  const page = useUi((s) => s.page);
  const Page = PAGES[page];
  return (
    <div className="relative isolate flex h-full">
      <div className="arcade-grid pointer-events-none absolute inset-0 -z-10" />
      <div className="pointer-events-none absolute inset-0 -z-10 bg-[radial-gradient(ellipse_at_top_right,rgba(255,179,71,0.08),transparent_50%),radial-gradient(ellipse_at_bottom_left,rgba(62,230,255,0.05),transparent_50%)]" />
      <Sidebar />
      <main className="relative flex flex-1 flex-col overflow-y-auto">
        <div className="drag sticky top-0 z-10 h-9 shrink-0" data-tauri-drag-region />
        <div className="mx-auto w-full max-w-4xl px-10 pb-12">
          {state.error ? <div className="text-destructive">{t("app.stateError", { error: errorText(state.error) })}</div> : <Page />}
        </div>
      </main>
    </div>
  );
}
