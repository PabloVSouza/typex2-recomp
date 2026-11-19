import { PanelLeftOpen } from "lucide-react";
import { useTranslation } from "react-i18next";
import { Brand } from "@/components/sidebar/Brand";
import { useAppState } from "@/hooks";
import { cn } from "@/lib";
import { useUi } from "@/stores";

/** Full-screen mode: the logo where the sidebar shows it, with the button that brings the menu back. */
export function FloatingBrand() {
  const { t } = useTranslation();
  const { data } = useAppState();
  const show = useUi((s) => s.setMenuHidden);
  return (
    <div className={cn("no-drag fixed left-5 z-20 flex items-center gap-3", data?.os === "macos" ? "top-11" : "top-5")}>
      <Brand />
      <button
        type="button"
        onClick={() => show(false)}
        title={t("nav.showMenu")}
        aria-label={t("nav.showMenu")}
        className="rounded-lg p-1.5 text-muted-foreground transition hover:bg-white/5 hover:text-foreground [&_svg]:size-4"
      >
        <PanelLeftOpen />
      </button>
    </div>
  );
}
