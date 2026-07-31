# Releasing

How a commit becomes something somebody else can install, and how their copy
finds out about it.

---

## The short version

```
1. Bump VERSION in CMakeLists.txt, commit it.
2. git tag v0.3.0 && git push origin v0.3.0
3. Wait. The Release workflow does the rest.
```

Everything below is why each of those steps is there.

---

## What the workflow does

`.github/workflows/release.yml`, triggered by a tag matching `v*`.

1. **Checks the tag against `CMakeLists.txt`** and refuses to go on if they
   disagree. This is the first step that can fail, and deliberately: a release
   whose manifest says 0.3.0 while the binary reports 0.2.0 produces an editor
   that offers the same update every time it is asked and installs it to no
   effect. Nothing later in the pipeline would notice.
2. Builds the `ui` preset with warnings as errors — the whole tree, FFmpeg and
   Skia included.
3. Runs the tests, and then `cutline --check`, which lays out and paints every
   panel in every theme. A release that ships a control nobody can reach is
   worse than a release that does not happen.
4. `cpack -G NSIS` produces `Cutline-<version>-Setup.exe`.
5. Computes the installer's SHA-256 and writes `latest.json` beside it.
6. Creates the GitHub release and uploads **both** files.

The build is slow — FFmpeg and Skia from source are over an hour on a cold
vcpkg cache. It shares its cache key with the nightly build, so a release cut
the day after a nightly starts warm.

---

## The manifest

```json
{
  "version": "0.3.0",
  "installer": "https://github.com/.../releases/download/v0.3.0/Cutline-0.3.0-Setup.exe",
  "sha256": "…64 hex characters…",
  "notes": "Cutline 0.3.0"
}
```

The editor fetches it from
`https://github.com/kylegaskill89/cutline-native/releases/latest/download/latest.json`.
That URL never changes: GitHub answers `/releases/latest/download/<asset>` with
a redirect to whichever release is newest, so nothing has to enumerate releases
or parse an API.

**Both assets or neither.** A release with an installer and no manifest is one
where every editor's update check answers 404, which looks exactly like being
offline. The publish step uploads them together and uses `--clobber` so a
re-run replaces them rather than failing halfway.

`parse_release_manifest` rejects rather than tolerates, because what is on the
other end of it is an executable that will be run:

| Refused | Why |
|---|---|
| an installer URL that is not `https` | a plain-http one can be replaced in transit by anybody on the path |
| a missing or malformed `sha256` | an unverified download is a promise nobody made |
| a version that does not parse | a version that cannot be ordered is not newer, and must not pretend to be |

Fields it has never heard of are ignored, so a later release can add to the
manifest without stranding everyone running an older editor.

There is a test — `ReadsWhatTheReleaseWorkflowWrites` — that parses the exact
JSON the workflow produces, key order and spacing included. It exists because
nothing else joins a YAML file nobody compiles to a parser nobody runs by hand:
a field renamed on either side would otherwise be found by somebody's editor
saying "could not check for updates" a week after the release went out. There
is another for a UTF-8 byte order mark, which is what Windows PowerShell writes
and `pwsh` does not.

---

## What the editor does with it

Nothing, until asked. There is no background check and no telemetry: an editor
that phones home the moment it opens has decided on the user's behalf that it
may.

The version at the foot of the project panel is the button. Pressing it walks
one step each time:

- **check** — fetches the manifest, compares, and says either "up to date" or
  what is available, with the release notes;
- **download** — fetches the installer on a worker, showing a percentage on the
  button;
- **install** — verifies the digest, asks about anything unsaved, starts the
  installer and closes the editor.

The digest is checked **before the file touches the disk**. A download that
failed it is never worth having: one left lying beside a real installer is an
invitation, and writing it first and deleting it after leaves a window where it
exists.

The installer runs `cutline.exe` on its finish page, so the editor comes back
after an update rather than leaving the user staring at a desktop.

---

## Cutting a release by hand

Sometimes the workflow is not what you want — a rebuild of an existing tag, or
a release from a machine with a warm cache.

```
cmake --preset ui
cmake --build --preset ui
ctest --test-dir build/ui -C Release
./build/ui/tools/cutline/Release/cutline.exe --check

cd build/ui
cpack -G NSIS -C Release
```

NSIS has to be installed (`choco install nsis`). Without it CPack fails at the
last step, after the long part.

Then compute the digest and write the manifest the same way the workflow does,
and upload both. There is no shortcut for this on purpose: the digest has to be
of the file that was actually uploaded, and one computed from anything else is
a check that passes for the wrong reasons or fails for no reason.

---

## What is packaged

The application is a *directory*, not a file: `cutline.exe`, the FFmpeg and
Skia runtimes beside it, and the seven compiled shaders the compositor opens by
name at startup. Missing any one of them is a build that starts and then cannot
do the thing it exists for — which is how the shaders were once found to be
missing from the window entirely.

The runtime files are globbed at install time rather than listed. Which DLLs
there are depends on how FFmpeg was configured, so a hand-written list is one
that goes stale silently, and the failure it produces is a missing codec at
export time rather than a build error. `cmake/Packaging.cmake` fails the
install outright if the glob finds nothing.
