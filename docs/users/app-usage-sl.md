# Navodila za uporabo

Sledite tem korakom za pretvorbo podatkov vašega posrednika v XML datoteko, primerno za oddajo na FURS, s pomočjo programa **TaxBrokerReport**.

---

## 1. Priprava vhodne datoteke

* Prijavite se v platformo svojega posrednika (Trade Republic).
* **Pridobite davčno poročilo**: V mobilni aplikaciji kliknite na svoj profil (zgoraj desno), izberite **Tax** in prenesite **Annual Tax Report**.
* Shranite datoteko PDF na svoj računalnik.
* **Alternativa (JSON)**: Če že imate podatke v podprtem [JSON formatu](data-format.md), lahko namesto PDF datoteke uporabite to.

## 2. Nalaganje podatkov

1. Zaženite aplikacijo **TaxBrokerReport**.
2. **Davčni podatki (Tax Data)**:
    * Vnesite svojo **davčno številko** (Tax Number).
    * Nastavite **davčno leto** (npr. 2025).
3. **Izbira obrazca (Form Selection)**:
    * **Vrsta obrazca (Form Type)**: Izberite ustrezno napoved (npr. **Doh-KDVP** za dobiček iz kapitala).
    * **Vrsta dokumenta (Document Type)**: Običajno **Original** (razen če oddajate popravek).
4. **Izbira vhoda (Input Selection)**:
    * Kliknite gumb **"Browse..."** ob polju **Input File**.
    * Izberite preneseno Trade Republic PDF datoteko ali svojo JSON datoteko.
5. **Izbira izhoda (Output Selection)**:
    * Kliknite gumb **"Browse..."** ob polju **Output Directory**.
    * Izberite mapo, kamor želite shraniti generirano XML datoteko.

## 3. Konfiguracija in neobvezni podatki

* **Način JSON (JSON Mode)**: Če želite le prebrati PDF in pridobiti surove podatke, označite **"Mode: Generate intermediate JSON only"**.
* **Neobvezni kontaktni podatki**: Vnesete lahko e-pošto ali telefonsko številko, če želite, da sta vključeni v glavo XML datoteke za potrebe kontakta s strani FURS.

## 4. Izdelava XML datoteke

1. Kliknite gumb **"Generate XML"** na dnu aplikacije.
2. Program bo obdelal vhodne podatke in shranil XML datoteko v izbrano mapo.
3. Preverite statusno vrstico (ali pojavno okno), da se prepričate o uspešnosti postopka.

---

## 🔒 Zasebnost in varnost

* **Lokalna obdelava**: Aplikacija za delovanje ne potrebuje internetne povezave.
* **Brez zbiranja podatkov**: Vaši finančni podatki ostanejo na vaši napravi. Med postopkom se razvijalcu ali kateri koli tretji osebi ne pošiljajo nikakršne informacije.

---

## 🛠 Odpravljanje težav

* **Težave z branjem PDF**: Prepričajte se, da uporabljate uradni "Annual Tax Report" posrednika Trade Republic. Drugi izpiski ali potrdila o naročilih niso podprti. Ker imamo za testiranje na voljo omejeno število vzorcev davčnih poročil, se pri določenih postavitvah lahko pojavijo napake pri branju.
* **Davčna številka**: Prepričajte se, da vnesete točno 8 številk brez presledkov ali posebnih znakov.
* **Manjkajoče transakcije**: Če se zdi, da transakcije manjkajo, preverite, ali so se dejansko zgodile v izbranem davčnem letu.
* **Ročni popravki**: Če je generiran XML napačen, lahko uporabite **JSON Mode**, da prenesete surove podatke. JSON datoteko lahko nato ročno uredite in jo naložite nazaj v aplikacijo (namesto PDF), da generirate končni, popravljeni XML.

---

## 🖼️ Izgled aplikacije

Spodaj je posnetek zaslona glavnega vmesnika za lažje prepoznavanje ključnih polj:

* **Zgornji del**: Zavihki za navigacijo (Domov, Navodila, Vizitka).
* **Srednji del**: Vnos davčnih podatkov in izbira datotek.
* **Spodnji del**: Gumb "Generate XML" za izdelavo datoteke.

![TaxBrokerReport GUI](../assets/GUImain.png)

![TaxBrokerReport GUI](../assets/GUImanuals.png)

---

## ⚖️ Naslednji koraki

Ko imate pripravljeno XML datoteko, nadaljujte na [Vodnik za oddajo na eDavki](edavki-upload-sl.md) za oddajo napovedi na FURS.
