import { CheckCircle2, CircleDashed, Download } from "lucide-react";
import { useTranslation } from "react-i18next";
import { Row } from "@/components/common";
import { Badge, Button } from "@/components/ui";
import { toolHint, toolName, toolUse } from "@/lib/data";
import { bytes } from "@/lib";
import type { Package, Tool } from "@/lib";

/** A tool: found or not, where, and its download. */
export function ToolRow({ tool, pkg, os, busy, onDownload }: { tool: Tool; pkg?: Package; os: string; busy: boolean; onDownload: () => void }) {
  const { t } = useTranslation();
  const source = tool.source === "kit" ? t("tools.source.kit") : tool.source === "downloaded" ? t("tools.source.downloaded") : t("tools.source.system");
  const where = pkg ? (pkg.size ? t("tools.downloadSize", { size: bytes(pkg.size) }) : t("tools.downloadOr")) : "";
  return (
    <Row
      title={
        <span className="flex items-center gap-2">
          {tool.path ? <CheckCircle2 className="size-4 text-lime" /> : <CircleDashed className="size-4 text-muted-foreground" />}
          {toolName(t, tool)}
          {pkg && <span className="text-xs font-normal text-muted-foreground">{pkg.version}</span>}
        </span>
      }
      description={
        <>
          <span>{t("tools.for", { use: toolUse(t, tool) })}</span>{" "}
          {tool.path ? <span className="break-all font-mono">{tool.path}</span> : <span>{where} {toolHint(t, tool, os)}</span>}
        </>
      }
    >
      {tool.source && <Badge variant="secondary">{source}</Badge>}
      {pkg && tool.source !== "downloaded" && (
        <Button size="sm" variant="secondary" disabled={busy} onClick={onDownload}><Download /> {t("tools.download")}</Button>
      )}
    </Row>
  );
}
