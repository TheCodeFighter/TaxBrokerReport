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
  navAbout: { sl: "O aplikaciji", en: "About" },
  welcomeLine: {
    sl: "Iz izvozov posrednikov pripravi datoteke XML za eDavke.",
    en: "Turns broker exports into XML files for eDavki.",
  },
  welcomeStart: { sl: "Začni", en: "Start" },
  disclaimerTitle: { sl: "Opozorilo", en: "Disclaimer" },
  disclaimerText: {
    sl: "To je neuradno orodje skupnosti. Ni davčni nasvet in ni povezano s FURS ali s katerim koli posrednikom. Sami ste odgovorni za preverjanje vseh vrednosti v ustvarjenih datotekah in za oddajo napovedi v eDavkih. Program je na voljo »tak, kot je«, pod licenco MIT, brez kakršnega koli jamstva. Razvijalec ne odgovarja za davčne napake, globe ali opustitve.",
    en: "This is an unofficial community tool. It is not tax advice and is not affiliated with FURS or any broker. You are responsible for checking every value in the generated files and for filing the forms in eDavki. The software is provided \"as is\", under the MIT License, without warranty of any kind. The developer is not liable for tax errors, fines or omissions.",
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
  taxpayerErrorBirthDate: { sl: "Vnesite veljaven datum.", en: "Enter a valid date." },
  taxpayerErrorField: { sl: "Preverite to polje.", en: "Check this field." },
  taxpayerErrorGeneral: {
    sl: "Podatki zavezanca niso veljavni. Preverite polja.",
    en: "The taxpayer data is not valid. Check the fields.",
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
