import type { ReactNode } from "react";
import { Row } from "@/components/common/Row";
import { Switch } from "@/components/ui";

export function SwitchRow({ title, description, checked, onChange }: { title: ReactNode; description?: ReactNode; checked: boolean; onChange: (v: boolean) => void }) {
  return (
    <Row title={title} description={description}>
      <Switch checked={checked} onCheckedChange={(v) => onChange(v)} />
    </Row>
  );
}
