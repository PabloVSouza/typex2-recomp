import type { ReactNode } from "react";
import { Row } from "@/components/common/Row";
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from "@/components/ui";

export function SelectRow<T extends string | number>({ title, description, value, options, onChange }: {
  title: ReactNode; description?: ReactNode; value: T; options: { value: T; label: string }[]; onChange: (v: T) => void;
}) {
  return (
    <Row title={title} description={description}>
      <Select value={value} items={options} onValueChange={(v) => v !== null && onChange(v as T)}>
        <SelectTrigger className="w-full bg-secondary sm:w-auto sm:min-w-56">
          <SelectValue />
        </SelectTrigger>
        <SelectContent>
          {options.map((o) => (
            <SelectItem key={String(o.value)} value={o.value}>{o.label}</SelectItem>
          ))}
        </SelectContent>
      </Select>
    </Row>
  );
}
