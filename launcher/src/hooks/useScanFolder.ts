import { toast } from "sonner";
import { useTranslation } from "react-i18next";
import { useAction } from "@/hooks/useAction";
import { api, pickFolder } from "@/lib";

/** Asks for a folder of game folders and adds every supported game in it. */
export function useScanFolder() {
  const { t } = useTranslation();
  const scan = useAction(api.scanFolder);
  return async () => {
    const path = await pickFolder(t("library.pickScanFolder"));
    if (!path) return;
    scan.mutate(path, {
      onSuccess: (found) => {
        if (found.length === 0) toast.warning(t("library.scanNone"));
        else toast.success(t("library.scanFound", { count: found.length }), { description: found.map((g) => g.title).join(", ") });
      },
    });
  };
}
