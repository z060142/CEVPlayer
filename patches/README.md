# Patches

Engine patches for CEVPlayer, applied in numeric order by `tools/apply_patches.sh <engine-root>` (`--with-optional` adds the optional ones).
The goal is to keep the engine patch surface as small as possible: everything else is synced in as
plain files by `tools/sync_sources.sh`.

## Supported trees

| Variant | Tree | Base | Series |
|---|---|---|---|
| `vanilla` | CRYENGINE 5.7.1 | `86736a0` | `patches/NN-*.patch` |
| `community` | CECE (CryENGINE-5-7-community) | `e1326cf` | `patches/community/NN-*.patch` where one exists, else the main `patches/NN-*.patch` of that number |

`BASE_COMMIT` records the commits (informational). The tree is identified by `BASE_FILES.vanilla` /
`BASE_FILES.community`: `<git blob sha1>  <path>` for every file the series touches, hashed with CR
characters removed (`tr -d '\r' < file | git hash-object --stdin`), so CRLF and LF checkouts hash the
same. A file is accepted when it has its base hash or when one of the patches touching it is already
applied. An unknown tree is refused unless `--force` is passed.

The patches carry **CRLF line endings** inside their hunks, because the engine files are CRLF. They must
apply with plain `git apply` (`.gitattributes` keeps them out of line-ending normalisation). An engine
checkout converted to LF will not match; check out with `core.autocrlf=true` like the reference trees.

## Series

| Patch | Files | Purpose |
|---|---|---|
| `01-register-plugins.patch` | `Code/CryPlugins/CMakeLists.txt`, `Tools/CMake/BuildSandbox.cmake` | Registers the CEVPlayer engine plugin (`add_subdirectory(CEVPlayer/Module)`) and the future Sandbox plugin `EditorCEVPlayer`. Both are guarded with `EXISTS`, so a tree without the synced sources still configures. |
| `community/01-register-plugins.patch` | `Tools/CMake/BuildSandbox.cmake` | Same for CECE. CECE already contains an unguarded `add_subdirectory(CEVPlayer/Module)` in `Code/CryPlugins/CMakeLists.txt`, so only the Sandbox hunk is needed. |

## Optional patches

`patches/optional/NN-*.patch` (community override: `patches/community/optional/NN-*.patch`) are not part of the series. They are applied only with
`tools/apply_patches.sh <engine-root> --with-optional` (idempotent like the series; without the flag an applied optional patch is just reported and left
alone, and a tree that has one applied is still recognised through `BASE_FILES.*`). The plug-in builds and runs without them and falls back at run time.

| Patch | Files | Purpose |
|---|---|---|
| `optional/02-cryaudio-trigger-position.patch` | `CryCommon/CryAudio/IObject.h`, `CryAudioSystem/{Common/IObject.h, Object.h, Object.cpp, CryAudioSystem.cpp}`, `CryAudioSystem/implementations/CryAudioImplSDLMixer/{Common.h, Common.cpp, Event.cpp, Object.h, Object.cpp}` | Playback position of a CryAudio trigger for the cue route (M3). Applies cleanly to both trees (the ten files have identical base hashes), so there is no community variant. |

What it adds (everything is append-only):

* `CryAudio::IObject::GetTriggerPlaybackPosition(ControlId, float&) const`: a new virtual appended after the last existing virtual (outside the
  interfuscator shuffle block) with a default that returns `false`; the internal `Impl::IObject` gets the same appended virtual (default `false`) so Wwise /
  FMOD / Adx2 / PortAudio and the NULL audio system compile and behave exactly as before. `CryAudio::CObject` (final, internal to `CryAudioSystem.dll`) overrides
  the public one and forwards to the implementation object; no data member was added to any class, no vtable slot moved (existing, already built modules keep
  working; a plug-in built against the old header simply never calls the new slot). One caveat is inherent to appending a virtual to `Impl::IObject`: an
  *implementation* DLL that was compiled before the patch has a vtable without the new slot. `CryAudioSystem.dll` therefore calls the slot only when the loaded
  `CryAudioImplSDLMixer.dll` exports the marker `CEVPlayer_AudioImplPositionPatched` (looked up once per module; always true in monolithic builds); every other or
  stale implementation answers `false` without being called. Still rebuild the implementation targets you use after applying the patch.
* `CryAudioSystem.dll` exports `extern "C" int CEVPlayer_AudioPatched()` (bit mask, bit 0 = playback position query). The plug-in calls the new virtual only after
  `GetProcAddress(GetModuleHandle("CryAudioSystem.dll"), "CEVPlayer_AudioPatched")` succeeded. (A plug-in compiled against an unpatched header cannot name the new
  member at all: `Audio/CueRoute.cpp` detects that at compile time and then never calls it.)
* SDL_mixer implementation: **how the position is measured.** For every started event instance, `BeginPlaybackTracking()` registers a `Mix_RegisterEffect`
  callback on the channel *before* `Mix_PlayChannel`; SDL_mixer calls it from its mixing callback with the number of bytes it mixes from the sample (also for loops;
  a paused channel is not mixed, so the count stops). The bytes are accumulated and a steady-clock time stamp is stored in the same call. A query returns
  `bytes / (rate * frameBytes) - one output buffer (4096 frames) + min(time since the last callback, 2 buffers)`: the estimated audible position, smooth between
  the ~85 ms callbacks. The effect's done callback (channel finished / halted / `Mix_UnregisterEffect`) frees the tracker.
  `triggerId` cannot be used: SDL_mixer connections do not know the trigger they belong to, so the answer is the most recently started event instance of the object
  that is still playing (the plug-in uses one object per player and one trigger at a time).
* **Thread safety.** The tracker state lives in a fixed static array of `std::atomic` (new code in `Common.cpp`; it is *not* a member of an existing class and it is
  not part of any audio-thread container). Writers are serialised: the audio system thread initialises a free tracker while its channel is idle, afterwards only SDL
  runs the callbacks (effect and done callbacks both run with the SDL audio device lock held, `Mix_HaltChannel` too). The effect callback publishes
  `{bytes, time stamp}` with a seqlock; the main-thread reader scans the table, picks the live tracker of the object that started last, reads the pair with a bounded
  retry loop and re-validates the tracker (state / owner / order) afterwards: any inconsistency means "unknown" (`false`), never a wrong read. The query never waits for the
  audio thread and never touches `m_eventInstances`. `CObject::GetTriggerPlaybackPosition` reads only the implementation pointer, which is set under the blocking
  `CreateObject` request; the caller must not query an object it has passed to `ReleaseObject` (and in debug builds an audio-control refresh `s_Refresh` replaces
  it, a one-frame race that only a developer command can trigger).
* Internal SDL_mixer implementation classes were extended only with the tracker functions (`Common.h/.cpp`), one call pair in `CEvent::Execute` and the override in the
  impl `CObject` (internal to `CryAudioImplSDLMixer.dll`, not an exported / shared layout).

Apply and verify: `tools/apply_patches.sh D:/coding11/Cryengine_5.7.1 --with-optional`, rebuild `CryAudioSystem`, `CryAudioImplSDLMixer` (solution targets
`CRYENGINE\CryEngine\Audio\CryAudioSystem`, `CRYENGINE\CryEngine\Audio\Implementations\CryAudioImplSDLMixer`) or the whole solution, then
`dumpbin /exports bin/win_x64/CryAudioSystem.dll` lists `CEVPlayer_AudioPatched`. Remove it again with `git -C <engine> apply -R patches/optional/02-cryaudio-trigger-position.patch`.

## Usage

```bash
tools/apply_patches.sh D:/coding11/Cryengine_5.7.1           # idempotent: re-running skips applied patches
tools/apply_patches.sh D:/coding11/Cryengine_5.7.1 --with-optional   # also the optional patches (cue route audio position)
tools/apply_patches.sh D:/coding11/CryENGINE-5-7-community
tools/sync_sources.sh  <engine-root>                         # plugin sources, SDKs, CMake modules
```

Nothing is committed in the engine tree. To reset: `git -C <engine> checkout -- Code/CryPlugins/CMakeLists.txt Tools/CMake/BuildSandbox.cmake`.

## Adding a patch

1. Generate it against the pristine base (`git diff` in a scratch repo that contains copies of the base files,
   committed with `core.autocrlf=false`, edited while keeping CRLF).
2. If CECE differs in the touched files, add `patches/community/NN-<same topic>.patch`.
3. Add the touched files to `BASE_FILES.*` (hash of the pristine file, see above) unless already listed.
4. Check: `git -C <tree> apply --check <patch>` for both trees, then run `tools/apply_patches.sh` twice.
