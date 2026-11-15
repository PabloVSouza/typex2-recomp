import { useTranslation } from "react-i18next";
import { ExternalLink } from "lucide-react";
import Markdown from "react-markdown";
import { Button, Dialog, DialogClose, DialogContent, DialogDescription, DialogFooter, DialogHeader, DialogTitle } from "@/components/ui";
import { useOpenRelease } from "@/hooks";
import type { ReleaseInfo } from "@/lib";

// Release notes are text: no raw HTML, remote images or links that open inside the launcher.
const ELEMENTS = ["p", "h1", "h2", "h3", "h4", "h5", "h6", "ul", "ol", "li", "strong", "em", "code", "pre", "blockquote", "hr", "br"];

type Props = { release: ReleaseInfo; open: boolean; onOpenChange: (open: boolean) => void };

/** A release's notes, and its page (the downloads) in the browser. */
export function UpdateDialog({ release, open, onOpenChange }: Props) {
  const { t } = useTranslation();
  const openRelease = useOpenRelease();
  return (
    <Dialog open={open} onOpenChange={onOpenChange}>
      <DialogContent className="sm:max-w-xl">
        <DialogHeader className="pr-8">
          <DialogTitle>{t("updates.available", { version: release.version })}</DialogTitle>
          <DialogDescription>{t("updates.downloadHint")}</DialogDescription>
        </DialogHeader>
        <section aria-label={t("updates.notes")} tabIndex={0}
          className="max-h-[50vh] overflow-y-auto break-words text-sm leading-relaxed [&_p]:my-3 [&_h1]:my-3 [&_h1]:text-lg [&_h2]:my-3 [&_h2]:text-base [&_h3]:my-3 [&_h3]:font-semibold [&_ul]:my-3 [&_ul]:list-disc [&_ul]:pl-5 [&_ol]:my-3 [&_ol]:list-decimal [&_ol]:pl-5 [&_li]:my-1 [&_pre]:overflow-x-auto [&_pre]:rounded [&_pre]:bg-muted [&_pre]:p-3 [&_code]:font-mono [&_blockquote]:border-l-2 [&_blockquote]:pl-3">
          {release.notes.trim()
            ? <Markdown skipHtml allowedElements={ELEMENTS} unwrapDisallowed>{release.notes}</Markdown>
            : <p className="text-muted-foreground">{t("updates.noNotes")}</p>}
        </section>
        <DialogFooter>
          <DialogClose render={<Button variant="outline" />}>{t("updates.close")}</DialogClose>
          <Button disabled={openRelease.isPending} onClick={() => openRelease.mutate(release.tag)}>
            <ExternalLink />{t("updates.viewRelease")}
          </Button>
        </DialogFooter>
      </DialogContent>
    </Dialog>
  );
}
