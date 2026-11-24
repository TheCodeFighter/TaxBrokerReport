import type { Texts } from "../i18n/ui_texts";

/** One broker the user can add exports for. Each broker has its own area in the form. */
export interface BrokerKind {
  /** The `broker` value of the request (`trade_republic`). It also names the area. */
  broker: string;
  title: Texts;
  /** The `accept` filter of the file dialog (extensions and media types). */
  accept: string;
  /** Which export to pick in the broker's own application, shown under the title. */
  format: Texts;
}

/**
 * The brokers in the order of their source IDs: the files of the first broker are `source-0001`
 * and on, then the second broker's. A new broker is one more row; nothing else names a broker.
 */
export const brokerKinds: BrokerKind[] = [
  {
    broker: "trade_republic",
    title: { sl: "Trade Republic", en: "Trade Republic" },
    accept: ".csv,text/csv",
    format: { sl: "Izvoz transakcij (CSV)", en: "Transaction export (CSV)" },
  },
  {
    broker: "interactive_brokers",
    title: { sl: "Interactive Brokers", en: "Interactive Brokers" },
    accept: ".xml,application/xml,text/xml",
    format: { sl: "Flex Query (XML)", en: "Flex Query (XML)" },
  },
];
