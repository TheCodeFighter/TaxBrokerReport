# Actions manuals

## 🚀 Release & Build System

This repository uses a coordinated **Dispatcher-Worker** architecture to ensure all platform builds (Windows, Linux, macOS) are consolidated into a single GitHub Release.

### 🛠 How to Create a Release

1. **Production Release (Auto)**:
   - Push a git tag starting with `v` (e.g., `v1.2.0`).
   - The `TRIGGER ALL BUILDS` workflow starts automatically.
   - It creates one consolidated release and triggers all platform workers to upload their files.

2. **Manual/Beta Build**:
   - Go to the **Actions** tab -> **TRIGGER ALL BUILDS**.
   - Click **Run workflow** and enter a version name (or use the default `manual-build`).
   - This creates a "Pre-release" containing all platform assets.

### 🧪 Internal Testing (Standalone)

If you want to test a single platform without creating a public release:

- Go to the specific workflow (e.g., `Windows x86-64 Release`).
- Run it manually.
- **Result**: The `.zip` will appear only at the bottom of that specific Action run under **Artifacts**.
- **Note**: These test artifacts are automatically deleted after **5 days** to save storage.

### 🧹 Maintenance & Cleanup

To keep the repository clean from old manual tests:

- Go to **Actions** -> **Cleanup Manual Releases**.
- Click **Run workflow**.
- **Result**: It identifies and deletes all releases and git tags containing the string `manual-build`.

### 📋 CI/CD Architecture Summary

| Component | Responsibility |
| :--- | :--- |
| **Dispatcher** | Creates the Release "Room" and manages versioning logic. |
| **Workers** | Compile code and upload portable ZIPs to the Release ID. |
| **Internal Storage** | Stores builds for 5 days for private testing. |
| **Cleanup Bot** | Manually removes "manual-build" clutter from the Release page. |
