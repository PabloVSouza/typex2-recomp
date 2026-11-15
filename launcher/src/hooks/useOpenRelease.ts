import { useMutation } from "@tanstack/react-query";
import { toast } from "sonner";
import { i18n } from "@/i18n";
import { api } from "@/lib";

/** A release's page in the system's browser. */
export function useOpenRelease() {
  return useMutation({ mutationFn: api.openRelease, onError: () => toast.error(i18n.t("updates.openFailed")) });
}
