import appIcon from "@/assets/app-icon.png";

/** The app icon (the pixel art at 2x, launcher/icon) and the name. */
export function Brand() {
  return (
    <div className="flex items-center gap-2.5">
      <img src={appIcon} alt="" className="size-9 [image-rendering:pixelated]" />
      <div className="leading-tight">
        <div className="font-display text-sm font-bold tracking-[0.18em]">TYPE X</div>
        <div className="font-display text-[10px] tracking-[0.3em] text-muted-foreground">RECOMP</div>
      </div>
    </div>
  );
}
