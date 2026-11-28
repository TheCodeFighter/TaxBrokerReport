TaxBrokerReport @VERSION@
=========================

SLOVENŠČINA
-----------

TaxBrokerReport pretvori izvoze borznih posrednikov (Trade Republic, Interactive Brokers) v
slovenske obrazce FURS (XML za eDavke): Doh-KDVP, Doh-Div, Doh-DHO in Doh-Obr. Vse ostane na
vašem računalniku, nič se ne pošlje po spletu.

Zagon
  V terminalu: ./taxbroker_server (AppImage: dvoklik na datoteko; paket deb ima vnos v meniju)
  Program odpre brskalnik. Če se brskalnik ne odpre, v terminalu izpisano povezavo kopirajte
  vanj. Povezava deluje enkrat in 2 minuti.

Izklop
  Gumb »Izklopi program« v strani, ali Ctrl+C v terminalu, ali zaprtje terminalskega okna.
  Program se izklopi tudi sam: 10 sekund po zaprtju zadnjega zavihka (osvežitev strani ga ne
  izklopi), če se nihče ne prijavi v 3 minutah ali če ga 60 minut ne uporabljate.

Ukazna vrstica
  ./taxbroker report --help
  ./taxbroker --version
  ./TaxBrokerReport-@VERSION@-x86_64.AppImage report --help   (AppImage)

Podatki in dnevnik (program jih ustvari sam)
  ~/.local/share/taxbroker/           shranjeni podatki davčnega zavezanca (samo če to izberete)
  ~/.local/state/taxbroker/           dnevnik taxbroker.log (brez zneskov, imen in poti)
  Ob odstranitvi programa ti dve mapi izbrišite sami, če ju ne potrebujete več.

Zavrnitev odgovornosti
  To je neuradno orodje skupnosti. Ni davčni nasvet in ni povezano s FURS ali s katerim koli
  posrednikom. Sami ste odgovorni za preverjanje vseh vrednosti v ustvarjenih datotekah in za
  oddajo napovedi v eDavkih. Program je na voljo »tak, kot je«, pod licenco MIT, brez
  kakršnega koli jamstva. Razvijalec ne odgovarja za davčne napake, globe ali opustitve.

Licenca: LICENSE (MIT). Licence kode drugih avtorjev: THIRD_PARTY_NOTICES.txt.
Vprašanja in napake: tax.brokerage.report@gmail.com, https://github.com/TheCodeFighter/TaxBrokerReport


ENGLISH
-------

TaxBrokerReport turns broker exports (Trade Republic, Interactive Brokers) into Slovenian FURS
tax forms (XML for eDavki): Doh-KDVP, Doh-Div, Doh-DHO and Doh-Obr. Everything stays on your
computer; nothing is sent over the internet.

Start
  In a terminal: ./taxbroker_server (the AppImage: a double click on its file; the deb package
  has a menu entry)
  The program opens your browser. If it does not open, copy the link printed in the terminal
  into it. A link works once and for 2 minutes.

Stop
  The "Quit the program" button in the page, or Ctrl+C in the terminal, or closing the terminal
  window. The program also stops by itself: 10 seconds after you close its last browser tab (a
  reload does not stop it), when nobody logs in within 3 minutes, or when it is not used for 60
  minutes.

Command line
  ./taxbroker report --help
  ./taxbroker --version
  ./TaxBrokerReport-@VERSION@-x86_64.AppImage report --help   (AppImage)

Data and log (the program makes these itself)
  ~/.local/share/taxbroker/           the saved taxpayer details (only if you choose that)
  ~/.local/state/taxbroker/           the log taxbroker.log (no amounts, names or paths)
  When you remove the program, delete these two folders yourself if you no longer need them.

Disclaimer
  This is an unofficial community tool. It is not tax advice, and it is not affiliated with FURS
  or any broker. You are responsible for checking every value in the generated files and for
  filing your return in eDavki. This software is provided "as is" under the MIT License, without
  warranty of any kind. The developer assumes no liability for tax errors, fines or omissions.

Licence: LICENSE (MIT). The licences of the third-party code: THIRD_PARTY_NOTICES.txt.
Questions and problems: tax.brokerage.report@gmail.com, https://github.com/TheCodeFighter/TaxBrokerReport
