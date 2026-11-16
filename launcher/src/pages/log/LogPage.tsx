import { useEffect, useRef, useState } from "react";
import { Bug, Eraser } from "lucide-react";
import { useTranslation } from "react-i18next";
import { PageHeader } from "@/components/common";
import { useReport } from "@/hooks";
import { Button, Tabs, TabsList, TabsTrigger } from "@/components/ui";
import { useActivity } from "@/stores";

export function LogPage() {
  const { t } = useTranslation();
  const [which, setWhich] = useState<"game" | "build">("game");
  const lines = useActivity((s) => (which === "game" ? s.gameLog : s.buildLog));
  const clear = useActivity((s) => s.clear);
  const report = useReport();
  const end = useRef<HTMLDivElement>(null);
  useEffect(() => {
    end.current?.scrollIntoView({ block: "end" }); /* in braces: newer engines return a promise, which React would take for a cleanup */
  }, [lines.length]);
  return (
    <>
      <PageHeader
        kicker={t("nav.log")}
        title={t("log.title")}
        actions={
          <>
            <Tabs value={which} onValueChange={(v) => setWhich(v as "game" | "build")}>
              <TabsList>
                <TabsTrigger value="game">{t("log.game")}</TabsTrigger>
                <TabsTrigger value="build">{t("log.builds")}</TabsTrigger>
              </TabsList>
            </Tabs>
            <Button variant="secondary" onClick={() => clear(which)}><Eraser /> {t("log.clear")}</Button>
            <Button variant="secondary" onClick={report} title={t("report.hint")}><Bug /> {t("report.button")}</Button>
          </>
        }
      />
      <div className="surface-inset h-[calc(100dvh-12rem)] overflow-auto rounded-xl p-4 font-mono text-[11.5px] leading-relaxed text-muted-foreground select-text">
        {lines.length === 0 ? <div>{which === "game" ? t("log.gameEmpty") : t("log.buildEmpty")}</div> : lines.map((l, i) => <div key={i} className="whitespace-pre-wrap break-all">{l}</div>)}
        <div ref={end} />
      </div>
    </>
  );
}
