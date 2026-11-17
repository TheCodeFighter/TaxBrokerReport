export type Language = "sl" | "en";

export interface Texts {
  sl: string;
  en: string;
}

/**
 * Every text of the interface itself, in both languages. A missing language is a type error.
 * The diagnostic messages are not here: they come from the server (`/api/messages`).
 */
export const uiTexts = {
  appName: { sl: "Tax Broker Report", en: "Tax Broker Report" },
  languageLabel: { sl: "Jezik", en: "Language" },
  themeToLight: { sl: "Preklopi na svetlo temo", en: "Switch to the light theme" },
  themeToDark: { sl: "Preklopi na temno temo", en: "Switch to the dark theme" },
  navLabel: { sl: "Glavni meni", en: "Main menu" },
  navHome: { sl: "Domov", en: "Home" },
  navReport: { sl: "Poročilo", en: "Report" },
  navManual: { sl: "Navodila", en: "Manual" },
  navSupport: { sl: "Podpora", en: "Support" },
  navAbout: { sl: "O aplikaciji", en: "About" },
  welcomeLine: {
    sl: "Iz izvozov posrednikov pripravi datoteke XML za eDavke.",
    en: "Turns broker exports into XML files for eDavki.",
  },
  welcomeStart: { sl: "Začni", en: "Start" },
  disclaimerTitle: { sl: "Opozorilo", en: "Disclaimer" },
  disclaimerText: {
    sl: "To je neuradno orodje skupnosti. Ni davčni nasvet in ni povezano s FURS ali s katerim koli posrednikom. Sami ste odgovorni za preverjanje vseh vrednosti v ustvarjenih datotekah in za oddajo napovedi v eDavkih. Program je na voljo »tak, kot je«, pod licenco MIT, brez kakršnega koli jamstva. Razvijalec ne odgovarja za davčne napake, globe ali opustitve.",
    en: "This is an unofficial community tool. It is not tax advice, and it is not affiliated with FURS or any broker. You are responsible for checking every value in the generated files and for filing your return in eDavki. This software is provided \"as is\" under the MIT License, without warranty of any kind. The developer assumes no liability for tax errors, fines or omissions.",
  },
  manualTitle: { sl: "Navodila", en: "Manual" },
  manualLater: {
    sl: "Navodila bodo na voljo v eni od naslednjih različic.",
    en: "The manual comes in a later version.",
  },
  aboutTitle: { sl: "O aplikaciji", en: "About" },
  versionLabel: { sl: "Različica {version}", en: "Version {version}" },
  aboutLicense: {
    sl: "Odprtokodni program pod licenco MIT. Celotno besedilo je v datoteki LICENSE ob programu.",
    en: "Open-source software under the MIT License. The full text is in the LICENSE file beside the program.",
  },
  aboutPrivacy: {
    sl: "Vaše datoteke ostanejo na tem računalniku. Program teče lokalno in ničesar ne pošilja na internet.",
    en: "Your files stay on this computer. The program runs locally and sends nothing to the internet.",
  },
  aboutSource: {
    sl: "Izvorna koda, prijava napak in predlogi:",
    en: "Source code, bug reports and suggestions:",
  },
  aboutSourceLink: { sl: "projekt na GitHubu", en: "the project on GitHub" },
  aboutThirdParty: {
    sl: "Program uporablja odprtokodne komponente; njihove licence so v datoteki THIRD_PARTY_NOTICES.txt ob programu.",
    en: "The program uses open-source components; their licenses are in the THIRD_PARTY_NOTICES.txt file beside the program.",
  },
  checkingTitle: { sl: "Povezovanje …", en: "Connecting …" },
  checkingBody: {
    sl: "Preverjam, ali je aplikacija pripravljena.",
    en: "Checking that the application is ready.",
  },
  filesTitle: { sl: "Datoteke", en: "Files" },
  brokerFilesHint: {
    sl: "Dodajte vse izvoze tega posrednika. Vrstni red določa številko datoteke v sporočilih.",
    en: "Add every export of this broker. The order sets the file number in the messages.",
  },
  chooseFiles: { sl: "Izberi datoteke", en: "Choose files" },
  noFilesChosen: { sl: "Ni izbranih datotek.", en: "No files chosen." },
  fileSuffix: { sl: " (datoteka {n})", en: " (file {n})" },
  moveUp: { sl: "Premakni više", en: "Move up" },
  moveDown: { sl: "Premakni niže", en: "Move down" },
  removeFile: { sl: "Odstrani", en: "Remove" },
  otherFilesTitle: { sl: "Druge datoteke (neobvezno)", en: "Other files (optional)" },
  rateFilesTitle: { sl: "Datoteke s tečaji ECB", en: "ECB exchange rate files" },
  rateFilesHint: {
    sl: "Za posle, ki niso v EUR. Tečaji za leta {first} do {last} so že v aplikaciji.",
    en: "For trades not in EUR. The rates of the years {first} to {last} are already in the application.",
  },
  referenceFilesTitle: { sl: "Referenčne datoteke", en: "Reference files" },
  referenceFilesHint: {
    sl: "Datoteke s podatki o izplačevalcih in dogovorih o izogibanju dvojnemu obdavčevanju.",
    en: "Files with payer data and tax treaty texts.",
  },
  yearTitle: { sl: "Leto poročanja", en: "Reporting year" },
  yearRateHint: {
    sl: "Za posle, ki niso v EUR, dodajte datoteko s tečaji ECB za to leto.",
    en: "For trades not in EUR, add the ECB rate file of this year.",
  },
  filingTitle: { sl: "Vrsta vloge", en: "Type of filing" },
  filingOriginal: { sl: "Izvirna vloga", en: "Original" },
  filingSelfReport: { sl: "Samoprijava", en: "Self-report" },
  taxpayerTitle: { sl: "Davčni zavezanec", en: "Taxpayer" },
  taxpayerResidentNote: {
    sl: "Podprti so samo slovenski davčni rezidenti.",
    en: "Only Slovenian tax residents are supported.",
  },
  taxpayerTaxNumber: { sl: "Davčna številka", en: "Tax number" },
  taxpayerName: { sl: "Ime in priimek", en: "Name" },
  taxpayerAddress: { sl: "Naslov", en: "Address" },
  taxpayerPostCode: { sl: "Poštna številka", en: "Post code" },
  taxpayerPostName: { sl: "Pošta", en: "Post office" },
  taxpayerCity: { sl: "Kraj", en: "City" },
  taxpayerBirthDate: { sl: "Datum rojstva", en: "Date of birth" },
  taxpayerEmail: { sl: "E-pošta", en: "Email" },
  taxpayerPhone: { sl: "Telefon", en: "Phone" },
  taxpayerErrorTaxNumber: {
    sl: "Vnesite osem števk davčne številke.",
    en: "Enter the eight digits of the tax number.",
  },
  taxpayerHintTaxNumberCheckDigit: {
    sl: "Kontrolna številka ni pravilna. Preverite davčno številko; poročila lahko kljub temu ustvarite.",
    en: "The check digit is not correct. Check the tax number; you can still create the reports.",
  },
  taxpayerErrorBirthDate: { sl: "Vnesite veljaven datum.", en: "Enter a valid date." },
  taxpayerErrorField: { sl: "Preverite to polje.", en: "Check this field." },
  taxpayerErrorGeneral: {
    sl: "Podatki zavezanca niso veljavni. Preverite polja.",
    en: "The taxpayer data is not valid. Check the fields.",
  },
  advancedTitle: { sl: "Napredno", en: "Advanced" },
  logLevelLabel: { sl: "Raven dnevnika", en: "Log level" },
  logLevelError: { sl: "Samo napake", en: "Errors only" },
  logLevelWarn: { sl: "Napake in opozorila", en: "Errors and warnings" },
  logLevelInfo: { sl: "Koraki (info)", en: "Steps (info)" },
  logLevelDebug: { sl: "Vse (debug)", en: "Everything (debug)" },
  logLevelHint: {
    sl: "Dnevnik ne vsebuje zneskov, imen ali imen datotek. Priložite ga le, če vas za to prosi razvijalec: izberite »Koraki« in ponovite izračun. Raven velja do izklopa programa. Datoteka taxbroker.log je v mapi ~/.local/state/taxbroker.",
    en: "The log holds no amounts, names or file names. Send it only when the developer asks for it: choose \"Steps\" and run again. The level lasts until the program is closed. The file taxbroker.log is in the folder ~/.local/state/taxbroker.",
  },
  logLevelFailed: {
    sl: "Ravni dnevnika ni bilo mogoče spremeniti.",
    en: "The log level could not be changed.",
  },
  profileRemember: { sl: "Zapomni si moje podatke", en: "Remember my details" },
  profileHint: {
    sl: "Podatki ostanejo samo na tem računalniku, v: {location}",
    en: "Stays on this computer only, in: {location}",
  },
  profileHintNoLocation: {
    sl: "Podatki ostanejo samo na tem računalniku, v podatkovni mapi aplikacije.",
    en: "Stays on this computer only, in the app's data folder.",
  },
  profileWarning: {
    sl: "Shranjeni podatki bodo izbrisani s tega računalnika.",
    en: "Saved details will be deleted from this computer.",
  },
  profileCancel: { sl: "Prekliči", en: "Cancel" },
  profileConfirm: { sl: "Izbriši", en: "Confirm delete" },
  profileNotDeleted: {
    sl: "Shranjenih podatkov ni bilo mogoče izbrisati.",
    en: "The saved details could not be deleted.",
  },
  profileNotSaved: {
    sl: "Vaših podatkov ni bilo mogoče shraniti.",
    en: "Your details could not be saved.",
  },
  profileUnreadable: {
    sl: "Shranjenih podatkov ni bilo mogoče prebrati, zato niso uporabljeni.",
    en: "The saved details could not be read and were not used.",
  },
  profileInvalidFields: {
    sl: "Nekateri shranjeni podatki niso veljavni in so ostali prazni: {fields}.",
    en: "Some saved details were not valid and were left empty: {fields}.",
  },
  profileIgnoredKeys: {
    sl: "Shranjena datoteka je vsebovala druge podatke; ti so prezrti.",
    en: "The saved file held other data; it was ignored.",
  },
  createReports: { sl: "Ustvari poročila", en: "Create the reports" },
  createReportsAgain: { sl: "Ustvari poročila znova", en: "Create the reports again" },
  sending: { sl: "Obdelujem …", en: "Working …" },
  runErrorNoBrokerFile: {
    sl: "Dodajte vsaj eno datoteko s posli.",
    en: "Add at least one file with trades.",
  },
  runErrorTooManyFiles: {
    sl: "Datotek je preveč. Naenkrat jih je lahko največ {n}.",
    en: "There are too many files. At most {n} at a time.",
  },
  runErrorBroker: {
    sl: "Posrednik ni podprt.",
    en: "The broker is not supported.",
  },
  runErrorGeneral: {
    sl: "Zahteve ni bilo mogoče obdelati. Osvežite stran in poskusite znova.",
    en: "The request could not be processed. Reload the page and try again.",
  },
  runErrorTooLarge: {
    sl: "Datoteka je prevelika. Ena datoteka je lahko velika največ {n} MB.",
    en: "A file is too large. One file can be at most {n} MB.",
  },
  runErrorTooLargeNamed: {
    sl: "Datoteka {name} je prevelika. Ena datoteka je lahko velika največ {n} MB.",
    en: "The file {name} is too large. One file can be at most {n} MB.",
  },
  runErrorFault: { sl: "Datoteke ni mogoče prebrati: {name}.", en: "A file cannot be read: {name}." },
  runErrorFaultNoSource: { sl: "Datoteke ni mogoče prebrati.", en: "A file cannot be read." },
  runErrorFaultRow: { sl: "vrstica {row}", en: "row {row}" },
  runErrorUnexpected: {
    sl: "Strežnik je odgovoril nepričakovano. Poskusite znova.",
    en: "The server gave an unexpected answer. Try again.",
  },
  resultTitle: { sl: "Rezultat", en: "Result" },
  resultOutOfDate: {
    sl: "Podatki so se spremenili. Rezultat je zastarel: ustvarite poročila znova.",
    en: "The inputs changed. This result is out of date: create the reports again.",
  },
  resultFiles: { sl: "Datoteke s posli", en: "Trade files" },
  supportTitle: { sl: "Podpora", en: "Support" },
  supportIntro: {
    sl: "Če nekaj ne deluje, shranite poročilo za podporo in ga priložite prijavi težave. Opisuje zadnji zagon: različico programa, stanje vsakega obrazca in kode sporočil. Najprej ustvarite poročila na strani Poročilo.",
    en: "If something does not work, save the support report and attach it to your problem report. It describes the last run: the program version, the status of each form and the message codes. Create the reports on the Report page first.",
  },
  supportNoRun: {
    sl: "Ni zagona, ki bi ga lahko opisali. Najprej ustvarite poročila na strani Poročilo.",
    en: "There is no run to describe yet. Create the reports on the Report page first.",
  },
  supportReportSave: { sl: "Shrani poročilo za podporo", en: "Save support report" },
  supportReportHint: {
    sl: "Za prijavo težave. Ne vsebuje zneskov, imen, davčnih številk ali imen datotek. Preberite ga, preden ga pošljete.",
    en: "For a problem report. It holds no amounts, names, tax numbers or file names. Read it before you send it.",
  },
  resultCounts: { sl: "Napake: {errors}, opozorila: {warnings}", en: "Errors: {errors}, warnings: {warnings}" },
  statusSuccess: { sl: "Vsa poročila so pripravljena.", en: "All reports are ready." },
  statusCompletedWithWarnings: {
    sl: "Poročila so pripravljena, z opozorili.",
    en: "The reports are ready, with warnings.",
  },
  statusPartialSuccess: { sl: "Nekatera poročila so pripravljena.", en: "Some reports are ready." },
  statusNeedsInput: { sl: "Potrebujemo še vaš odgovor.", en: "We need an answer from you." },
  answersLater: {
    sl: "Odgovarjanje na vprašanja v brskalniku pride v naslednji različici. Zaenkrat jih zastavi ukazna vrstica (taxbroker report).",
    en: "Answering the questions in the browser comes in a later version. For now the command line (taxbroker report) asks them.",
  },
  statusFailed: { sl: "Poročil ni bilo mogoče pripraviti.", en: "The reports could not be made." },
  reportGenerated: { sl: "pripravljeno", en: "ready" },
  reportNoData: { sl: "ni podatkov", en: "no data" },
  reportNeedsInput: { sl: "čaka na odgovor", en: "waiting for an answer" },
  reportFailed: { sl: "ni uspelo", en: "failed" },
  reportNoDataBelowThreshold: {
    sl: "Obrazec ni potreben: obresti ne presegajo 1.000 EUR.",
    en: "No form needed: the interest is not above EUR 1,000.",
  },
  reportNoDataNoSales: {
    sl: "Obrazec ni potreben: v tem letu ni bilo prodaje.",
    en: "No form needed: no sale in this year.",
  },
  reportNoDataEmpty: { sl: "Za ta obrazec ni podatkov.", en: "No data for this form." },
  questionsWaiting: {
    sl: "Vprašanja, ki čakajo na odgovor: {count}",
    en: "Questions waiting for an answer: {count}",
  },
  resolutionExclude: {
    sl: "Vaša izbira: neuspešne naložbe so izpuščene.",
    en: "Your choice: the failed investments are left out.",
  },
  resolutionFail: {
    sl: "Vaša izbira: za ta obrazec ni datoteke XML.",
    en: "Your choice: no XML file for this form.",
  },
  resolutionDeveloper: { sl: "Vaša izbira: razvijalski izvoz.", en: "Your choice: developer export." },
  excludedTitle: { sl: "Izpuščene naložbe", en: "Investments left out" },
  excludedBrokersTitle: { sl: "Izpuščeni posredniki", en: "Brokers left out" },
  unsafeMarker: { sl: "Nevaren razvijalski izvoz", en: "Unsafe developer export" },
  unsafeTitle: {
    sl: "Naložbe z nepopolno zgodovino v tem izvozu",
    en: "Investments with incomplete history in this export",
  },
  evidenceRows: {
    sl: "Vrstice, za katere v eDavkih priložite dokazilo o plačanem tujem davku: {count}",
    en: "Rows that need the proof of the foreign tax attached in eDavki: {count}",
  },
  sharedMessagesTitle: { sl: "Sporočila za več poročil", en: "Messages for several reports" },
  classificationTitle: { sl: "Izpuščeno pri razvrstitvi", en: "Left out by classification" },
  excludedReasonDebtSecurity: { sl: "dolžniški vrednostni papir", en: "debt security" },
  excludedReasonCrypto: { sl: "kriptosredstvo", en: "crypto" },
  excludedReasonSlovenianTax: {
    sl: "davek je odtegnil slovenski izplačevalec",
    en: "the Slovenian payer withheld the tax",
  },
  excludedReasonUserChoice: { sl: "vaša izbira", en: "your choice" },
  severityWarning: { sl: "Opozorilo", en: "Warning" },
  severityError: { sl: "Napaka", en: "Error" },
  severityErrorKept: { sl: "Napaka (izračuna ne ustavi več)", en: "Error, kept (no longer blocks)" },
  stageParsing: { sl: "Branje datotek", en: "Reading the files" },
  stageMerging: { sl: "Združevanje datotek", en: "Combining the files" },
  stageHistory: { sl: "Preverjanje zgodovine", en: "Checking the history" },
  stageCorporateAction: { sl: "Korporacijski dogodki", en: "Corporate actions" },
  stageFifoTax: { sl: "FIFO in davek", en: "FIFO and tax" },
  stageXml: { sl: "Priprava XML", en: "Making the XML" },
  fieldLabel: { sl: "polje {field}", en: "field {field}" },
  groupCount: { sl: "{count}-krat", en: "{count} times" },
  detailsSummary: { sl: "Podrobnosti ({count})", en: "Details ({count})" },
  sourceFile: { sl: "datoteka {n}", en: "file {n}" },
  sourceRow: { sl: "vrstica {row}", en: "row {row}" },
  sourceTransaction: { sl: "ID transakcije {id}", en: "transaction ID {id}" },
  sessionEndedTitle: { sl: "Odprite povezavo znova", en: "Open the link again" },
  sessionEndedBody: {
    sl: "Seja je končana ali aplikacija ni več zagnana. Zaženite aplikacijo in odprite novo povezavo, ki jo izpiše.",
    en: "The session has ended or the application is no longer running. Start the application and open the new link that it prints.",
  },
  linkInvalidTitle: { sl: "Povezava ne velja več", en: "The link is no longer valid" },
  linkInvalidBody: {
    sl: "Povezava deluje samo enkrat in 2 minuti. Ustavite aplikacijo, jo znova zaženite in odprite novo povezavo, ki jo izpiše.",
    en: "A link works once and for 2 minutes. Stop the application, start it again and open the new link that it prints.",
  },
  runChanged: {
    sl: "Podatki so se spremenili. Osvežite stran in poskusite znova.",
    en: "The data changed. Reload the page and try again.",
  },
  unknownMessage: { sl: "Neznano sporočilo", en: "Unknown message" },
} satisfies Record<string, Texts>;

export type UiTextKey = keyof typeof uiTexts;

export function uiText(language: Language, key: UiTextKey): string {
  return uiTexts[key][language];
}

/** A text with its `{name}` places filled, for example `{n}` with a number. */
export function uiTextWith(
  language: Language,
  key: UiTextKey,
  values: Record<string, string | number>,
): string {
  return uiText(language, key).replace(/\{(\w+)\}/g, (place, name: string) => String(values[name] ?? place));
}
