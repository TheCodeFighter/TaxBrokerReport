import type { ComponentChildren } from "preact";

interface Props {
  /** The ID of the heading: the region is named by it. */
  id: string;
  title: string;
  /** A short line under the heading. */
  hint?: string;
  children: ComponentChildren;
}

/** One framed part of the result with its own heading: shared messages, fees, tax, files. */
export function ResultRegion({ id, title, hint, children }: Props) {
  return (
    <section class="result-region" aria-labelledby={id}>
      <h3 id={id}>{title}</h3>
      {hint === undefined ? null : <p class="hint">{hint}</p>}
      {children}
    </section>
  );
}
