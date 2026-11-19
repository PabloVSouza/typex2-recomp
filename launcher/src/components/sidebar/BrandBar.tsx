import { PanelLeftClose, PanelLeftOpen } from "lucide-react";
import { useTranslation } from "react-i18next";
import { Brand } from "@/components/sidebar/Brand";
import { useAppState, useNarrow } from "@/hooks";
import { cn } from "@/lib";
import { useUi } from "@/stores";

/** The logo and the button that hides or shows the menu (full-screen mode), fixed where the sidebar
 *  starts: neither moves while the sidebar slides. Upright (an icon rail) there is no button. */
export function BrandBar() {
  const { t } = useTranslation();
  const { data } = useAppState();
  const { menuHidden, setMenuHidden } = useUi();
  const narrow = useNarrow();
  const label = menuHidden ? t("nav.showMenu") : t("nav.hideMenu");
  return (
    <div
      className={cn("drag fixed left-5 z-30 flex items-center gap-3 max-md:left-3.5", data?.os === "macos" ? "top-11" : "top-5")}
      data-tauri-drag-region
    >
      <Brand />
      {!narrow && (
        <button
          type="button"
          onClick={() => setMenuHidden(!menuHidden)}
          title={label}
          aria-label={label}
          className="no-drag ml-14 rounded-lg p-1.5 text-muted-foreground transition hover:bg-white/5 hover:text-foreground [&_svg]:size-4"
        >
          {menuHidden ? <PanelLeftOpen /> : <PanelLeftClose />}
        </button>
      )}
    </div>
  );
}
