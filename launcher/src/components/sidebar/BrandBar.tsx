import { PanelLeftClose, PanelLeftOpen } from "lucide-react";
import { useTranslation } from "react-i18next";
import { Brand } from "@/components/sidebar/Brand";
import { useAppState, useNarrow } from "@/hooks";
import { cn } from "@/lib";
import { useUi } from "@/stores";

/** The logo and the button that hides or shows the menu (full-screen mode), fixed where the sidebar
 *  starts: neither moves while the sidebar slides. Upright (an icon rail) there is no button. A
 *  controller reaches the button from the page or the sidebar (data-brandbar, spatialNav). */
export function BrandBar() {
  const { t } = useTranslation();
  const { data } = useAppState();
  const { menuHidden, menuPeek, setMenuHidden } = useUi();
  const shown = !menuHidden || menuPeek;
  const narrow = useNarrow();
  // shown for a moment by a controller (menuPeek): the button keeps it
  const label = shown && !menuPeek ? t("nav.hideMenu") : t("nav.showMenu");
  return (
    <div
      className={cn("drag fixed left-5 z-30 flex items-center gap-3 max-md:left-3.5", data?.os === "macos" ? "top-11" : "top-5")}
      data-tauri-drag-region
      data-brandbar
    >
      <Brand />
      {!narrow && (
        <button
          type="button"
          onClick={() => setMenuHidden(shown && !menuPeek)}
          title={label}
          aria-label={label}
          className="no-drag ml-14 rounded-lg p-1.5 text-muted-foreground transition hover:bg-white/5 hover:text-foreground [&_svg]:size-4"
        >
          {shown && !menuPeek ? <PanelLeftClose /> : <PanelLeftOpen />}
        </button>
      )}
    </div>
  );
}
