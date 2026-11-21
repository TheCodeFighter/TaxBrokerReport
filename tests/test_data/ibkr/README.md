# IBKR contract example

`contract_example.xml` is a **synthetic** Activity Flex Query XML file (accounts `U0000000` and
`U0000001`, made-up names and ISINs with valid check digits, no real value). It has one row for
each kind of row in the Interactive Brokers export contract (`docs/architecture.md`, "Interactive
Brokers export contract"), so the parser fixtures and the parser are built from it. On every trade,
`proceeds` = −`quantity` × `tradePrice` × `multiplier`.

The structure and the trade attributes are confirmed on a real export. The cash and corporate-action
attributes and their `type` values are not yet; they are corrected here and in the
contract when a real file shows a difference.

Account `U0000000` (`ibEntity` `IB-IE`):

| Element | `transactionID` | Kind | Expected event |
| --- | --- | --- | --- |
| `Trade` | 1001 | Buy of a share, fee in the trade currency | Buy 10 units at USD 150.25, fee USD 1.00 (fee currency USD), tax date 2024-03-05 |
| `Trade` | 1002 | Fractional sell (`FracShare`), fee in another currency, `notes` `P` | Sell 0.5 units at USD 200, fee EUR 0.35 (fee currency EUR) |
| `Lot` | 1001 | Closed lot of 1002, reuses the ID of buy 1001 | Ignored, one file-level info; no duplicate-ID conflict with 1001 |
| `Trade` | 1003 | Buy of an ETF, `notes` `IA` | Buy 4 units at EUR 50, fee EUR 1.25, fund |
| `Trade` | 1004 | Option sale (`OPT`, no ISIN, multiplier 100) | Derivative: left out of every form; counted in the one margin and leverage warning |
| `Trade` | 1006 | Currency conversion (`CASH`) | Ignored, info |
| `Trade` | 1007 | Cancelled trade (`BUY (Ca.)`, `TradeCancel`) | Error on that row |
| `Trade` | 1008 | Bond buy, 1000 nominal at 99.5 % (multiplier 0.01) | Bond: outside Doh-KDVP, listed in `excludedItems` |
| `Trade` | 1009 | Fund buy (`FUND`), fee 0 | Buy 3 units at EUR 100, fund |
| `Trade` | 1010 | Share buy with a financial transaction tax (`taxes` −0.8) | Buy 10 units at EUR 20, fee EUR 1.25 and a second fee EUR 0.80 |
| `Trade` | 1011 | Corrected trade (`TradeCorrect`) | Error on that row |
| `CashTransaction` | 2001, 2002, 2013 | Dividend (`A9001`), its withholding, a withholding refund booked in 2025 | One dividend, gross USD 12.00, tax USD 1.50 (1.80 − 0.30), 2024-03-15; warning: correction booked in a later year |
| `CashTransaction` | 2003 to 2005 | Dividend, its reversal and the corrected dividend (`A9002`), all booked in 2024 | One dividend, gross USD 4.50 (5.00 − 5.00 + 4.50), no tax, 2024-05-10 |
| `CashTransaction` | 2006 | Payment in lieu of dividends | Dividend USD 3.00 with a warning |
| `CashTransaction` | 2007 | Broker interest received | Interest EUR 3.21, broker cash, payer `IB-IE` |
| `CashTransaction` | 2008 | Broker interest paid (margin) | Left out; counted in the same warning (one warning for 1004 and 2008: 2 rows) |
| `CashTransaction` | 2009, 2010 | Bond coupon, accrued interest paid at the bond buy 1008 | Coupon EUR 45.00 (4.5 % of 1000, Doh-Obr); accrued-interest warning |
| `CashTransaction` | 2011, 2012 | Fee, deposit | Ignored (fee summary only) |
| `CorporateAction` | 3001 | Forward split 4 for 1 (`FS`), same ISIN: 9.5 units → 38 | Unresolved split, ratio asked |
| `CorporateAction` | 3004 | Reverse split 1 for 4 (`RS`), same ISIN: 4 units → 1 | Unresolved split, ratio asked |
| `CorporateAction` | 3002 | Merger (`TC`) | `unsupported_corporate_action` |
| `CorporateAction` | 3003 | Bond maturity (`BM`) | Redemption event, information only |
| `Transfer` | 4001, 4002 | Position transfers in and out | Section not read: no event, no diagnostic |

Account `U0000001` (`ibEntity` `IB-CE`), the same taxpayer:

| Element | `transactionID` | Kind | Expected event |
| --- | --- | --- | --- |
| `Trade` | 5001 | Buy of the same share as 1001 | Buy 2 units at USD 160; the same FIFO pool as account `U0000000` |
| `CorporateAction` | 5002 | The same split as 3001 (`S9001`): 2 units → 8 | One split for the ISIN, asked once |

Not in the example: `CRYPTO` and `CMDTY` trades. IBKR writes them without an ISIN, and the app
never makes one up; the parser task decides how such a row is named and left out.
