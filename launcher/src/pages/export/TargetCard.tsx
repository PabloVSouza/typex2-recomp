import { Monitor, Smartphone } from "lucide-react";
import { useTranslation } from "react-i18next";
import { targetName } from "@/lib/data";
import type { Target } from "@/lib";
import { cn } from "@/lib";

export function TargetCard({ target, active, onClick }: { target: Target; active: boolean; onClick: () => void }) {
  const { t } = useTranslation();
  return (
    <button
      type="button"
      onClick={onClick}
      className={cn("surface rounded-2xl p-4 text-left ring-1 transition", active ? "ring-cyan/60" : "ring-transparent hover:bg-white/[0.03]")}
    >
      <div className="mb-2 text-cyan [&_svg]:size-5">{target.id.startsWith("android") ? <Smartphone /> : <Monitor />}</div>
      <div className="font-display font-semibold">{targetName(t, target)}</div>
    </button>
  );
}
