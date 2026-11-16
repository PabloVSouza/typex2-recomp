import { useState } from "react";
import { useTranslation } from "react-i18next";
import { useMutation } from "@tanstack/react-query";
import { Send } from "lucide-react";
import { toast } from "sonner";
import { Button, Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from "@/components/ui";
import { useDevices } from "@/hooks";
import { api, errorText } from "@/lib";
import { useUi } from "@/stores";

/** An Android export sent to a connected device over adb: game packs to the Type X2 app, the core for RetroArch. */
export function SendToDevice({ target }: { target: string }) {
  const { t } = useTranslation();
  const go = useUi((s) => s.go);
  const { data } = useDevices();
  const [picked, setPicked] = useState<string | null>(null);
  const send = useMutation({
    mutationFn: (serial: string) => api.sendExport(target, serial),
    onSuccess: (sent) =>
      sent === "packs"
        ? toast.success(t("devices.packsSent"), { description: t("devices.packsSentText") })
        : toast.success(t("devices.copied"), { description: t("devices.copiedText") }),
    onError: (e) => toast.error(errorText(e)),
  });
  if (!data) return null;
  if (!data.adb) {
    return <Button variant="secondary" size="sm" onClick={() => go("tools")}>{t("devices.needAdb")}</Button>;
  }
  const ready = data.devices.filter((d) => d.state === "device");
  if (ready.length === 0) {
    const waiting = data.devices.some((d) => d.state === "unauthorized");
    return <span className="text-xs text-muted-foreground">{waiting ? t("devices.unauthorized") : t("devices.none")}</span>;
  }
  const serial = picked && ready.some((d) => d.serial === picked) ? picked : ready[0].serial;
  const items = ready.map((d) => ({ value: d.serial, label: d.model }));
  return (
    <div className="flex items-center gap-2">
      {ready.length > 1 && (
        <Select value={serial} items={items} onValueChange={(v) => v !== null && setPicked(v as string)}>
          <SelectTrigger size="sm" className="min-w-36 bg-secondary"><SelectValue /></SelectTrigger>
          <SelectContent>
            {items.map((i) => <SelectItem key={i.value} value={i.value}>{i.label}</SelectItem>)}
          </SelectContent>
        </Select>
      )}
      <Button size="sm" disabled={send.isPending} onClick={() => send.mutate(serial)}>
        <Send />{send.isPending ? t("devices.sending") : ready.length > 1 ? t("devices.send") : t("devices.sendTo", { device: ready[0].model })}
      </Button>
    </div>
  );
}
