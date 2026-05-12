# 🧪 Vodnik za Beta Testiranje (Davčno leto 2025)

Ker trenutno še čakamo, da Trade Republic izda uradna **letna davčna poročila za leto 2025** (običajno so na voljo v začetku leta 2026), moramo izvesti "stresni test" s preteklimi podatki.

Ta vodnik pojasnjuje, kako uporabiti vaše poročilo za leto 2024, da preverite, ali portal eDavki pravilno sprejme in izračuna naš generiran XML za sezono oddaje 2025. Aplikacija deluje tudi s poročili iz leta 2023, kar ponuja dodatno možnost testiranja.

---

## 🛠️ Seznam opravil pred testiranjem

1. **Različica aplikacije**: Prepričajte se, da uporabljate najnovejšo različico **TaxBrokerReport**.
2. **Davčna številka**: Vaša 8-mestna davčna številka v aplikaciji **se mora natančno ujemati** z osebo, ki je prijavljena v portal eDavki.
3. **App Setting**: V aplikaciji nastavite **Tax Year** na **2025**.

---

## 1. Kapitalski dobički (Doh-KDVP)

Ker bo FURS za to obdobje oddaje sprejel le posle z datumom v letu 2025, morate svoje posle iz leta 2024 ročno "poslati v prihodnost".

**Koraki:**

1. Generirajte XML z uporabo vašega Trade Republic PDF-ja za leto 2024.
2. Odprite XML v urejevalniku besedila (npr. Notepad++, VS Code ali Beležnica).
3. Uporabite funkcijo 'Najdi in zamenjaj' (ali ročno uredi), da spremenite vse datume **Prodaje** (`<F6>`) iz 2024 v 2025.

**Primer spremembe:**
*Od*:

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

*Do*:

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

## 2. Dividende (Doh-Div)

Podobno morajo datumi izplačil dividend pasti v koledarsko leto 2025, da jih portal sprejme.

**Koraki:**

1. Odprite generiran XML.
2. Posodobite polje `<Date>` za vsak vnos dividende iz 2024 v 2025.

**Primer spremembe:**
*Od*:

```Xml
<Dividend>
    <Date>2024-10-03</Date>
    <PayerIdentificationNumber>012345678</PayerIdentificationNumber>
    <PayerName>Example Corp</PayerName>
    ...
</Dividend>
```

*Do*:

```Xml
<Dividend>
    <Date>2025-10-03</Date>
    <PayerIdentificationNumber>012345678</PayerIdentificationNumber>
    <PayerName>Example Corp</PayerName>
    ...
</Dividend>
```

## 3. Obresti (Doh-DHO)

Za obrazec za obresti (DHO) ročno posodabljanje datumov za namene testiranja ni potrebno. Portal obravnava skupni znesek drugače kot datume posameznih transakcij. Preprosto naložite generiran XML takšen, kot je.

---

## 📤 Kako preveriti

1. Prijavite se v eDavke.
2. Uporabite funkcijo uvoza (Import), da naložite svoje spremenjene XML datoteke.
3. Preverite povzetek podatkov: Prepričajte se, da se izračuni ujemajo z vašimi pričakovanji in da portal ne javlja nobenih "rdečih" napak pri validaciji.

>[!TIP]
>Če je uvoz uspešen in izračun videti pravilen, je orodje pripravljeno na davčno sezono 2025! 🚀

---

## 🐛 Ste našli napako?

Če med testiranjem naletite na težave ali če portal eDavki zavrne generirano datoteko, nam to sporočite. Vaše povratne informacije pomagajo izboljšati orodje za vse uporabnike.

**Napake prijavite na:** [tax.brokerage.report@gmail.com](mailto:tax.brokerage.report@gmail.com)

**V e-poštno sporočilo vključite naslednje:**

* **Opis napake**: Kratka razlaga, kaj se je zgodilo.
* **Vrsta obrazca**: Kateri obrazec ste testirali? (KDVP, Div ali DHO).
* **Posnetki zaslona**: Posnetek morebitnih sporočil o napakah, ki jih prikažejo eDavki.
* **Broker PDF (Zelo priporočljivo)**: Če je mogoče, priložite originalno Trade Republic PDF poročilo. To nam omogoča, da ponovimo napako pri razčlenjevanju in popravimo logiko.
  > *Opomba: Svoje osebno ime in naslov lahko prekrijete/izbrišete, vendar prosimo, da pustite vidne finančne podatke in datume.*
