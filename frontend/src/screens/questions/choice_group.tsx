import { uiText, type Language } from "../../i18n/ui_texts";
import { choiceText } from "../../run/income_questions";

interface Props {
  language: Language;
  /** Unique on the page, so two questions never share one radio group. */
  name: string;
  legend: string;
  /** The words the server offers; a word the page has no text for is shown as it is. */
  options: string[];
  /** `undefined` until the user picks: nothing is preselected. */
  value: string | undefined;
  onChange: (word: string) => void;
}

/** One group of radio buttons for the choice words of a question. */
export function ChoiceGroup({ language, name, legend, options, value, onChange }: Props) {
  return (
    <fieldset class="field">
      <legend>{legend}</legend>
      {options.map((word) => {
        const key = choiceText(word);

        return (
          <label key={word} class="radio">
            <input type="radio" name={name} value={word} checked={value === word} onChange={() => onChange(word)} />
            {key === undefined ? word : uiText(language, key)}
          </label>
        );
      })}
    </fieldset>
  );
}
