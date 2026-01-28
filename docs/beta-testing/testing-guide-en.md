**# 🧪 Beta Testing Guide (2025 Tax Year)

As we are currently waiting for Trade Republic to release the official **2025 Annual Tax Reports** (usually available in early 2026), we need to perform "Stress Testing" using previous data.

This guide explains how to use your 2024 report to verify that the eDavki portal correctly accepts and calculates our generated XML for the 2025 filing season. The app also works with 2023 reports, which provides another testing option.

---

## 🛠️ Pre-test Checklist

1. **App Version**: Ensure you are using the latest release of **TaxBrokerReport**.
2. **Tax Number**: Your 8-digit tax number in the app **must exactly match** the person signed into the eDavki portal.
3. **App Setting**: In the application, set the **Tax Year** to **2025**.

---

## 1. Capital Gains (Doh-KDVP)

Since FURS will only accept trades with a 2025 date for this filing period, you must manually "time travel" your 2024 trades.

**Steps:**

1. Generate the XML using your 2024 Trade Republic PDF.
2. Open the XML in a text editor (e.g., Notepad++, VS Code, or Notepad).
3. Use 'Find and Replace' (or edit manually to be certain) to change all **Sale** dates (`<F6>`) from 2024 to 2025.

**Example Change:**
*From*:

```Xml
<Row>
    <ID>1</ID>
    <Sale>
        <F6>2024-04-09</F6>
        <F7>3.00000000</F7>
        <F9>10.00000000</F9>
        <F10>true</F10>
    </Sale>
    <F8>0.00000000</F8>
</Row>
```

*To*:

```Xml
<Row>
    <ID>1</ID>
    <Sale>
        <F6>2025-04-09</F6>
        <F7>3.00000000</F7>
        <F9>10.00000000</F9>
        <F10>true</F10>
    </Sale>
    <F8>0.00000000</F8>
</Row>
```

---

## 2. Dividends (Doh-Div)

Similarly, dividend payment dates must fall within the 2025 calendar year to be accepted by the portal.

**Steps:**

1. Open the generated XML.
2. Update the `<Date>` field for every dividend entry from 2024 to 2025.

**Example Change:**
*From*:

```Xml
<Dividend>
    <Date>2024-10-03</Date>
    <PayerIdentificationNumber>012345678</PayerIdentificationNumber>
    <PayerName>Example Corp</PayerName>
    ...
</Dividend>
```

*To*:

```Xml
<Dividend>
    <Date>2025-10-03</Date>
    <PayerIdentificationNumber>012345678</PayerIdentificationNumber>
    <PayerName>Example Corp</PayerName>
    ...
</Dividend>
```

## 3. Interests (Doh-DHO)

For the Interest form, no manual date updates are required for testing. The portal handles the aggregate amount differently than individual transaction dates. Simply upload the generated XML as-is.

---

## 📤 How to Verify

1. Log in to eDavki.
2. Use the Import feature to upload your modified XML files.
3. Check the Summary of Data: Ensure the calculations match your expectations and that the portal does not flag any "Red" validation errors.

>[!TIP]
>If the import is successful and the calculation looks correct, the tool is ready for the 2025 tax season! 🚀

---

## 🐛 Found a Bug?

If you encounter any issues during testing or if the eDavki portal rejects a generated file, please let us know. Your feedback helps improve the tool for everyone.

**Report issues to:** [tax.brokerage.report@gmail.com](mailto:tax.brokerage.report@gmail.com)

**Please include the following in your email:**

* **Error Description**: A brief explanation of what happened.
* **Form Type**: Which form were you testing? (KDVP, Div, or DHO).
* **Screenshots**: A capture of any error messages displayed by eDavki.
* **Broker PDF (Highly Recommended)**: If possible, attach the original Trade Republic PDF report. This allows us to reproduce the parsing error and fix the logic.
  > *Note: You may redact/black out your personal name and address, but please leave the financial figures and dates visible.*
**
