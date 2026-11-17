import { useEffect, useMemo, useState } from "preact/hooks";

import type { ApiClient } from "../api/client";
import { SessionEndedError } from "../api/client";
import { changeLogLevel, loadLogLevel, type LogLevel } from "../api/log_level";
import { deleteProfile, loadProfile } from "../api/profile";
import type { MessageCatalogue } from "../i18n/messages";
import { uiText, uiTextWith, type Language, type UiTextKey } from "../i18n/ui_texts";
import { emptyInputs, type RunInputs } from "../run/inputs";
import { inputLabels } from "../run/file_labels";
import {
  fillEmptyFields,
  formFromProfile,
  noticeText,
  profileLocationText,
  rememberBlocksRun,
  rememberBoxStep,
  type ProfileNotice,
  type RememberEvent,
  type RememberState,
} from "../run/profile";
import { markOutOfDate, type ShownResult } from "../run/result_view";
import { runErrorText, type ShownError } from "../run/run_errors";
import { submitRun } from "../run/submit";
import { RememberBox } from "./remember_box";
import { AdvancedSection } from "./advanced_section";
import { RunForm } from "./run_form";
import { RunResultView } from "./run_result";

interface Props {
  language: Language;
  apiFetch: ApiClient["apiFetch"];
  catalogue: MessageCatalogue | undefined;
}

/** The input screen of a run and, after a run, its result. One button sends the run; no polling. */
export function RunScreen({ language, apiFetch, catalogue }: Props) {
  const today = useMemo(() => new Date(), []);
  const [inputs, setInputs] = useState<RunInputs>(() => emptyInputs(today));
  const [sending, setSending] = useState(false);
  const [error, setError] = useState<ShownError | undefined>();
  const [shown, setShown] = useState<ShownResult | undefined>();
  // The saved profile: whether a file exists, where it is, the box and the notices about it.
  const [profileSaved, setProfileSaved] = useState(false);
  const [location, setLocation] = useState<string | undefined>();
  const [remember, setRemember] = useState<RememberState>("off");
  const [loadNotices, setLoadNotices] = useState<ProfileNotice[]>([]);
  // A failed save or delete of the last action; cleared by the next one.
  const [profileProblem, setProfileProblem] = useState<UiTextKey | undefined>();
  // The level of the log: read when the form opens, sent when the user changes it.
  const [logLevel, setLogLevel] = useState<LogLevel | undefined>();
  const [logLevelFailed, setLogLevelFailed] = useState(false);

  // The saved details load once after the login; they fill only the inputs still empty.
  useEffect(() => {
    let cancelled = false;

    loadProfile(apiFetch).then(
      (profile) => {
        if (cancelled || profile === undefined) {
          return;
        }

        const loaded = formFromProfile(profile);

        setProfileSaved(profile.saved);
        setLocation(profile.location);
        setRemember(loaded.remember);
        setLoadNotices(loaded.notices);
        setInputs((current) => ({ ...current, taxpayer: fillEmptyFields(current.taxpayer, loaded.form) }));
      },
      // A lost session is shown by the page itself; the form just stays empty.
      () => undefined,
    );

    return () => {
      cancelled = true;
    };
  }, [apiFetch]);

  useEffect(() => {
    let cancelled = false;

    loadLogLevel(apiFetch).then(
      (level) => {
        if (!cancelled) {
          setLogLevel(level);
        }
      },
      // A lost session is shown by the page itself; the select stays off.
      () => undefined,
    );

    return () => {
      cancelled = true;
    };
  }, [apiFetch]);

  // A change goes to the server at once. The select shows it first; on a failure it goes back.
  const chooseLogLevel = async (level: LogLevel) => {
    const previous = logLevel;

    setLogLevel(level);
    setLogLevelFailed(false);

    try {
      const now = await changeLogLevel(apiFetch, level);

      setLogLevelFailed(now === undefined);
      setLogLevel(now ?? previous);
    } catch (caught) {
      setLogLevel(previous);

      // A lost session is shown by the page itself.
      if (!(caught instanceof SessionEndedError)) {
        setLogLevelFailed(true);
      }
    }
  };

  // Any change of the inputs marks the shown result as out of date and clears the last error.
  const change = (changed: RunInputs) => {
    setInputs(changed);
    setError(undefined);
    setShown(markOutOfDate);
  };

  const send = async () => {
    const sent = inputs;
    const suffix = (number: number) => uiTextWith(language, "fileSuffix", { n: number });

    setSending(true);
    setError(undefined);

    try {
      const outcome = await submitRun(apiFetch, sent, remember === "on");

      // The save comes before the run, so it counts also when the run itself failed.
      if (outcome.profile === "saved") {
        // The new file holds only the checked fields, so the notices about the loaded file are over.
        setProfileSaved(true);
        setLoadNotices([]);
        setProfileProblem(undefined);
      } else if (outcome.profile === "not_saved") {
        setProfileProblem("profileNotSaved");
      }

      if (outcome.kind === "result") {
        setShown({ result: outcome.result, fileLabels: inputLabels(sent, suffix).broker, outOfDate: false });
      } else {
        setError(runErrorText(outcome, language, catalogue));
      }
    } catch (caught) {
      // A lost session is shown by the page itself; any other failure is an unexpected answer.
      if (!(caught instanceof SessionEndedError)) {
        setError({ text: uiText(language, "runErrorUnexpected") });
      }
    } finally {
      setSending(false);
    }
  };

  const step = (event: RememberEvent) => setRemember((state) => rememberBoxStep(state, event, profileSaved));

  const confirmDelete = async () => {
    step("confirm");
    setProfileProblem(undefined);

    try {
      if (await deleteProfile(apiFetch)) {
        setProfileSaved(false);
        setLoadNotices([]);
        step("deleted");
      } else {
        setProfileProblem("profileNotDeleted");
        step("deleteFailed");
      }
    } catch (caught) {
      step("deleteFailed");

      // A lost session is shown by the page itself.
      if (!(caught instanceof SessionEndedError)) {
        setProfileProblem("profileNotDeleted");
      }
    }
  };

  const notices = [
    ...loadNotices.map((notice) => noticeText(notice, language)),
    ...(profileProblem === undefined ? [] : [uiText(language, profileProblem)]),
  ];

  return (
    <>
      <RunForm
        language={language}
        today={today}
        inputs={inputs}
        onChange={change}
        fieldError={error?.field}
        sending={sending}
        outOfDate={shown?.outOfDate === true}
        submitBlocked={rememberBlocksRun(remember)}
        rememberBox={
          <RememberBox
            language={language}
            state={remember}
            hint={profileLocationText({ location }, language)}
            notices={notices}
            onTick={() => step("tick")}
            onUntick={() => step("untick")}
            onCancel={() => step("cancel")}
            onConfirm={() => void confirmDelete()}
          />
        }
        advanced={
          <AdvancedSection
            language={language}
            level={logLevel}
            failed={logLevelFailed}
            onChange={(level) => void chooseLogLevel(level)}
          />
        }
        onSubmit={() => void send()}
      />
      <div role="alert">{error === undefined ? null : <p class="error">{error.text}</p>}</div>
      {shown === undefined ? null : <RunResultView language={language} shown={shown} catalogue={catalogue} />}
    </>
  );
}
