# Vodnik za oddajo na eDavki

Ta vodnik vas bo vodil skozi postopek uvoza generiranih XML datotek v portal FURS eDavki.

---

## 1. Dostop do strani za uvoz

Po prijavi v namizni portal eDavki morate navigirati do splošnega razdelka za uvoz, namesto da bi odprli posamezen obrazec.

**Korak 1:** Poiščite vstopno točko za dokumente na vaši nadzorni plošči.

![eDavki Nadzorna plošča](../assets/GUInamizje.png)

**Korak 2:** V meniju kliknite na možnost **Uvoz dokumenta**.

![Meni za uvoz](../assets/GUIuvoz.png)

---

## 2. Nalaganje XML datoteke

Izberite XML datoteko, ki jo je generirala aplikacija **TaxBrokerReport**.

**Korak 3:** Kliknite **Prebrskaj** (Browse), izberite svojo datoteko in potrdite uvoz.

![Izbira datoteke](../assets/GUIuvozIzberiDatoteko.png)

---

## 3. Navodila za specifične obrazce

### A. Dobiček iz kapitala (Doh-KDVP)

**Korak 4:** Po uspešnem uvozu boste videli strukturo podatkov KDVP.

![KDVP naloženo](../assets/GUIkdvp1.png)

**Korak 5:** Preglejte povzetek vseh uvoženih transakcij.

![KDVP povzetek](../assets/GUIkdvp2.png)

> [!WARNING]
> **Pomembno opozorilo glede FIFO pravila in več posrednikov:**
> * **FIFO pravilo**: FURS zahteva metodo "prvi noter, prvi ven" (First-In, First-Out). Ta aplikacija generira podatke strogo na podlagi vašega Trade Republic poročila.
> * **Več posrednikov**: Če trgujete z **isto ISIN kodo** pri več posrednikih (npr. Revolut ali IBKR), posameznih datotek ne smete preprosto uvoziti ločeno. Podatke morate združiti, da zagotovite pravilno kronološko FIFO zaporedje prek vseh računov.
> * **Logika razčlenjevanja**: Aplikacija uporablja razdelek "Gains and Losses" iz poročila Trade Republic, kjer so pari nakupov in prodaj že usklajeni. To deluje brezhibno, če z dotičnimi sredstvi trgujete izključno preko Trade Republic.
> * **Postopek ročnega popravljanja**: Če želite dodati manjkajoče podatke ali združiti transakcije, lahko v aplikaciji najprej uporabite "JSON Mode" za generiranje JSON datoteke. To datoteko nato ročno uredite in jo naložite nazaj v aplikacijo (namesto PDF-ja) za izdelavo končnega XML-ja. Podrobna navodila najdete v [priročniku za JSON format](data-format.md).

**Korak 6:** Zdaj lahko poročilo shranite, naložite dodatna dokazila (PDF potrdila) ali oddate dokument.

![KDVP zaključek](../assets/GUIkdvp3.png)

---

### B. Dividende (Doh-Div)

**Korak 7:** Preverite uvožene zapise o dividendah.

![Dividende naloženo](../assets/GUIdiv1.png)

* **Samoprijava**: Če dokument oddajate med 1. januarjem in zadnjim dnem februarja, **ne odkljukajte** polje "Samoprijava".

**Korak 8:** Dopolnite manjkajoče podatke o plačniku.

![Dividende manjkajoči podatki](../assets/GUIdiv2.png)

* **Ročni vnosi**: Ročno morate vnesti plačnikov **Naslov**, **Državo** in **Državo vira**.
* **Preprečevanje dvojnega obdavčevanja**: Če uveljavljate znižanje na podlagi davka, ki je bil že plačan v tujini, morate označiti polje za uveljavljanje ugodnosti iz mednarodnih pogodb.

**Korak 9:** Dokazilo o davku, plačanem v tujini.

![Dokazilo o davku](../assets/GUIdiv3.png)

* Če ste uveljavljali odbitek tujega davka, **morate** kot prilogo naložiti uradni davčni izpisek vašega posrednika.

---

### C. Obresti na denarne depozite (Doh-DHO)

**Korak 10:** Preverite uvoz podatkov o obrestih.

![DHO naloženo](../assets/GUIdho1.png)

**Korak 11:** Preglejte zneske obresti.

![DHO podrobnosti](../assets/GUIdho2.png)

* **Meja 1000 €**: Ta obrazec morate oddati le, če vaše skupne obresti (na depozite) pri vseh bankah s sedežem v EU (Trade Republic, N26, itd.) v davčnem letu presegajo **1000 €**.
* **Združevanje**: Če imate obresti pri drugih bankah (npr. N26), jih ne pozabite dodati v to poročilo (kliknite dodaj zapis).

---

## ⚖️ Končna odgovornost

XML datoteka je pripomoček za prihranek časa. Vendar pa pravna odgovornost za točnost podatkov (in ročno dodajanje podatkov o plačniku pri dividendah) v celoti ostane na strani uporabnika.