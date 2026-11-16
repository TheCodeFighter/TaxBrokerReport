import { render } from "preact";

import { App } from "./app";
import { readTheme } from "./theme/theme";
import "./theme.css";
import "./app.css";

// The saved theme is set before the first render, so the page does not flash the other theme.
document.documentElement.dataset.theme = readTheme(() => window.localStorage);

const root = document.getElementById("app");

if (root !== null) {
  render(<App />, root);
}
