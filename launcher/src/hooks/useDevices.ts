import { useQuery } from "@tanstack/react-query";
import { api } from "@/lib";

/** Android devices adb sees, refreshed every few seconds while a page shows them. */
export function useDevices() {
  return useQuery({ queryKey: ["devices"], queryFn: api.listDevices, refetchInterval: 3000 });
}
