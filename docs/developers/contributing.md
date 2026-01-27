# 🤝 Contributing Guide

Thank you for helping Slovenian taxpayers sleep better at night! Here is how we work.

---

## 🔄 The Workflow

1. **Issue First**: Every PR should link to an issue, if we have it.

2. **Branching**: Create a branch from `main` with increment number of branch and *TR* prefix (i.e. TR-0100). When creating push branch for others seeing and incrementing their branch number.

3. **Pull Request**: Submit your PR. The `prep-pr.yml` bot will check if you linked an issue (e.g., `Fixes #42`).

---

## 🤖 CI/CD Pipelines

We use GitHub Actions to maintain quality and automate releases.

### 1. PR Checks (*test-pr.yml*)

Runs on every push to a PR. It:

* Builds the project.
* Runs all Unit Tests.
* Checks formatting (Clang-Format).

### 2. Release System (The "Dispatcher")

We use a **Dispatcher-Worker** architecture to build for all OSs simultaneously.

| Component | Responsibility |
| :--- | :--- |
| **Dispatcher** | Detects a `v*` tag, creates the GitHub Release, and orders workers to start. |
| **Workers** | Windows/Mac/Linux runners compile the code and upload ZIPs to the Release. |

**How to trigger a Release:**
Simply push a tag:

```bash
git tag v1.2.0
git push origin v1.2.0
```

Or manually in GitHub desktop. 

>[!Note]
>Currently only on exclusive domain of *TheCodeFighter*.

## 📝 Coding Standards

- **Language:** C++20.
  
- **Testing:** If you add logic, add a test in tests/. We aim for high coverage on the backend logic.

### 🔍 Technical "Why"

* **Separation of Docs**: Keeping "Building" separate from "Architecture" helps new contributors find the command they need (`make dev-up`) without scrolling through diagrams.
* **Docker Emphasis**: Since C++ dependencies (Qt6, Poppler) are painful to set up on Windows/Mac, emphasizing the **Dev Container** workflow drastically reduces the "it works on my machine" friction.
