import { useMutation, useQueryClient } from "@tanstack/react-query";
import { toast } from "sonner";
import { errorText } from "@/lib";

/** Runs a command, refreshes the state afterwards, and shows its error as a toast. */
export function useAction<A, R = unknown>(fn: (a: A) => Promise<R>, success?: string) {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: fn,
    onSuccess: () => success && toast.success(success),
    onError: (e) => toast.error(errorText(e)),
    onSettled: () => qc.invalidateQueries({ queryKey: ["state"] }),
  });
}
