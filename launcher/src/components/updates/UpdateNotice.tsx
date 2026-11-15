import { useState } from "react";
import { useTranslation } from "react-i18next";
import { ScrollText, X } from "lucide-react";
import { Alert, AlertDescription, AlertTitle, Button } from "@/components/ui";
import { UpdateDialog } from "@/components/updates/UpdateDialog";
import { useRelease } from "@/hooks";

/** A banner while a newer release is out; dismissed until the next one. */
export function UpdateNotice() {
  const { t } = useTranslation();
  const { data: release } = useRelease();
  const [dismissed, setDismissed] = useState<string | null>(null);
  const [notes, setNotes] = useState(false);
  if (!release?.newer || dismissed === release.tag) return null;
  return (
    <>
      <Alert className="mb-6 bg-primary/10">
        <AlertTitle>{t("updates.available", { version: release.version })}</AlertTitle>
        <AlertDescription className="flex flex-wrap items-center justify-between gap-3">
          <span>{t("updates.downloadHint")}</span>
          <div className="flex items-center gap-2">
            <Button size="sm" onClick={() => setNotes(true)}><ScrollText />{t("updates.notes")}</Button>
            <Button size="icon-sm" variant="ghost" aria-label={t("updates.dismiss")} onClick={() => setDismissed(release.tag)}><X /></Button>
          </div>
        </AlertDescription>
      </Alert>
      <UpdateDialog release={release} open={notes} onOpenChange={setNotes} />
    </>
  );
}
