import { useState } from "react";
import { useTranslation } from "react-i18next";
import { RefreshCw } from "lucide-react";
import { Row, Section, SwitchRow } from "@/components/common";
import { Button } from "@/components/ui";
import { UpdateDialog } from "@/components/updates";
import { useAction, useRelease } from "@/hooks";
import { api } from "@/lib";

/** Checking GitHub for new releases: automatically (the default) or now. */
export function UpdatesSection({ version, automatic }: { version: string; automatic: boolean }) {
  const { t } = useTranslation();
  const release = useRelease();
  const save = useAction(api.saveUpdateCheck);
  const [notes, setNotes] = useState(false);
  const r = release.data;
  const status = release.isFetching ? t("updates.checking")
    : release.isError ? t("updates.unavailable")
    : r === undefined ? t("updates.notChecked")
    : r === null ? t("updates.noRelease")
    : r.newer ? t("updates.available", { version: r.version })
    : t("updates.current");
  return (
    <Section title={t("updates.title")}>
      <SwitchRow title={t("updates.automatic")} description={t("updates.automaticText")} checked={automatic} onChange={(v) => save.mutate(v)} />
      <Row title={t("updates.installed", { version })} description={status}>
        <div className="flex gap-2">
          {r?.newer && <Button size="sm" variant="outline" onClick={() => setNotes(true)}>{t("updates.notes")}</Button>}
          <Button size="sm" variant="outline" disabled={release.isFetching} onClick={() => release.refetch()}>
            <RefreshCw />{t("updates.checkNow")}
          </Button>
        </div>
      </Row>
      {r?.newer && <UpdateDialog release={r} open={notes} onOpenChange={setNotes} />}
    </Section>
  );
}
