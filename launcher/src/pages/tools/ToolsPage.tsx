import { Download, FolderOpen, Trash2, X } from "lucide-react";
import { useTranslation } from "react-i18next";
import { JobProgress, PageHeader, Row, Section } from "@/components/common";
import { Button } from "@/components/ui";
import { useAction, useAppState } from "@/hooks";
import { api, reveal } from "@/lib";
import { bytes } from "@/lib";
import { BuildSettings } from "@/pages/tools/BuildSettings";
import { ToolRow } from "@/pages/tools/ToolRow";

export function ToolsPage() {
  const { t } = useTranslation();
  const { data } = useAppState();
  const install = useAction(api.installTools);
  const remove = useAction(api.removeTools, t("tools.removed"));
  const cancel = useAction(api.cancelJob);
  if (!data) return null;
  const missing = data.tools.filter((x) => x.download && x.source !== "downloaded").map((x) => x.download!);
  return (
    <>
      <PageHeader
        kicker={t("nav.tools")}
        title={t("tools.title")}
        description={t("tools.description")}
        actions={
          data.job === "tools"
            ? <Button variant="secondary" onClick={() => cancel.mutate(undefined)}><X /> {t("common.cancel")}</Button>
            : missing.length > 0 && <Button disabled={!!data.job} onClick={() => install.mutate(missing)}><Download /> {t("tools.downloadAll")}</Button>
        }
      />
      <JobProgress match="tools" className="mb-5" />
      <Section>
        {data.tools.map((x) => {
          const pkg = data.packages.find((p) => p.id === x.download);
          return <ToolRow key={x.id} tool={x} pkg={pkg} os={data.os} busy={!!data.job} onDownload={() => pkg && install.mutate([pkg.id])} />;
        })}
      </Section>
      <Section title={t("tools.storage")}>
        <Row title={t("tools.downloaded")} description={t("tools.downloadedText", { size: bytes(data.tools_size) })}>
          <Button variant="secondary" size="sm" onClick={() => reveal(data.data)}><FolderOpen /> {t("common.show")}</Button>
          <Button variant="destructive" size="sm" disabled={!!data.job || data.tools_size === 0} onClick={() => remove.mutate(undefined)}><Trash2 /> {t("common.remove")}</Button>
        </Row>
      </Section>
      <BuildSettings key={`${data.ndk}\n${data.jobs}`} ndk={data.ndk} jobs={data.jobs} />
    </>
  );
}
