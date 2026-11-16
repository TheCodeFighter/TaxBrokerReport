import type { Texts } from "../i18n/ui_texts";

/** One broker the user can add exports for. Each broker has its own area in the form. */
export interface BrokerKind {
  /** The `broker` value of the request (`trade_republic`). It also names the area. */
  broker: string;
  title: Texts;
}

/**
 * The brokers in the order of their source IDs: the files of the first broker are `source-0001`
 * and on, then the second broker's. A new broker (Interactive Brokers) is one more row.
 */
export const brokerKinds: BrokerKind[] = [
  { broker: "trade_republic", title: { sl: "Trade Republic", en: "Trade Republic" } },
];
