# HANDOFF / MASTER NOTES

**Repo:** `Glockerz/roblox-external`
**Branch:** `arena/01a05dd9-roblox-external` (18 commits ahead of `main`; `main` untouched by request)
**Last commit:** `79a5dba`
**Target client:** `version-f5a60436d48947d3` (`0.736.0.7361348`)

A Roblox usermode **external** cheat. It never injects — it reads/writes the Roblox
process with `ReadProcessMemory` / `WriteProcessMemory` and draws its own transparent
DX11 + ImGui overlay on top.

---

## 1. Quick start

### Build
1. Open `roblox external.slnx` in Visual Studio.
2. **If you're on VS 2022:** the project asks for toolset `v145` (VS 2026). Change it to `v143`:
   ```powershell
   (Get-Content "roblox external\roblox external.vcxproj") -replace 'v145','v143' | Set-Content "roblox external\roblox external.vcxproj"
   ```
3. Set **Release** + **x64**, then Build (`Ctrl+Shift+B`).
4. Output: `x64\Release\roblox external.exe` (solution build) or
   `roblox external\x64\Release\roblox external.exe` (project build).

Requires the **Desktop development with C++** workload and the Windows 10/11 SDK.
No package manager — ImGui (1.91.1) and Clipper2 are vendored.
Libs linked: `d3d11 dxgi dwmapi shell32`.

> ⚠️ Release builds with **AVX2** (`EnableEnhancedInstructionSet`). Needs a ~2013+ CPU.
> If you hit an illegal-instruction crash, drop it to `StreamingSIMDExtensions2`.

### Run
1. Start **Roblox** (windowed or borderless — overlays can't draw over exclusive fullscreen).
2. Run the exe **as Administrator** (needed to open a handle to Roblox).
3. The menu **opens automatically**. Toggle with **HOME** (rebindable in the keybinds tab).

Roblox does **not** need to be running first — the overlay launches immediately and
attaches in the background, and re-attaches automatically if Roblox closes/reopens.

**Exiting:** the `X` in the title bar, or right-click the **tray icon** → exit.
There's also a real **taskbar / Alt-Tab entry** ("roblox external"). No Task Manager needed.

---

## 2. Feature status

Honest state. "Unverified" = written and wired, never confirmed working in-game.

| Feature | Status | Notes |
|---|---|---|
| Walkspeed | ✅ **Confirmed working** | Writes `Humanoid::Walkspeed`. The one known-good write path. |
| ESP boxes | ⚠️ Works, slightly high | Box bottom floats above the feet — see §4.2 |
| ESP extras (name/health/distance/tool/skeleton/china hat) | ❓ Unverified | |
| Chams (regular) | ❓ Unverified | Does **not** use the deleted mesh backend |
| Aimbot + FOV circle | ❓ Unverified | Needs a keybind set |
| Noclip | ❓ Unverified | Needs a keybind set |
| Hitbox expander | ❓ Unverified | |
| Inventory checker | ❓ Unverified | Hold key with cursor over a player |
| FOV changer | ❓ Unverified since fix | Radians bug fixed — see §4.3 |
| **Flight** | ❌ **Broken** | 4 approaches tried — see §4.1 |
| **Infinite jump** | ❌ **Broken** | Same root cause as flight |
| **Click teleport** | ⚠️ Partly working | TP'd to cursor in 1st person; 3rd person + windowed fixed but unverified |
| **Skybox changer** | ❌ **Broken** | Invalidation-order fix applied, unverified — see §4.4 |
| Config save/load/rename | ❓ Unverified | Files under `GetConfigDir()` |

**Removed on request:** Blade Ball, Rivals skin changer, Phantom Forces special-casing,
korblox/rage, 3D ESP preview, mesh chams + memory mesh chams (see §5).

---

## 3. Architecture

```
roblox external/
  main.cpp        entry (WinMain), FeatureLoop thread, AttachLoop (auto-reconnect), render_ui()
  overlay.hpp     overlay window, D3D11, ImGui init + theme, input thread, tray icon, taskbar window
  overlay.h       thin accessor for the D3D device/context
  menu.cpp        entire GUI: ui:: widgets, nav bar, all pages
  globals.h       every setting as an inline global + LogLine() + g_request_exit
  memory.h/.cpp   RPM/WPM wrappers, instance struct, name/classname/children readers
  process.h/.cpp  FindRoblox(), GetRobloxWindow()
  game.h/.cpp     ReadDatamodel(), GetGameName(), GetPlaceId()
  cache.h/.cpp    background thread (16ms) publishing immutable snapshots
  offsets.h       all offsets for the target client version
  features/       aimbot click_teleport config esp flight fov_changer
                  hitbox_expander infinite_jump inventory_checker noclip
                  skybox_changer walkspeed
```

### Threads
| Thread | Job |
|---|---|
| main / render | `discord_overlay::run()` — pumps messages, draws ImGui, owns click-through |
| cache | re-reads DataModel → Players → characters every 16ms, publishes snapshots |
| feature | `FeatureLoop()` — runs all `Run*` features every 1ms |
| attach | `AttachLoop()` — attach + auto-reconnect, 1s poll |
| input | `input_thread()` — `GetAsyncKeyState` polling, feeds ImGui, menu toggle |

> The namespace is still called `discord_overlay` for historical reasons. It has
> **nothing to do with Discord any more** — that dependency was removed in `a18a7f1`.
> Renaming it is a safe, purely cosmetic cleanup.

### Data flow
`g_base_address + VisualEngine::Pointer` → `VisualEngine::FakeDataModel` →
`FakeDataModel::RealDataModel` → services by **class name** → players → characters → parts.

Rendering features must be called from `render_ui()` (render thread);
memory-only features from `FeatureLoop()`.

---

## 4. Open problems (with what's already been ruled out)

### 4.1 Flight + infinite jump — THE blocker

Four mechanisms tried, all failed:

| # | Approach | Result |
|---|---|---|
| 1 | Write `AssemblyLinearVelocity` | Nothing happens |
| 2 | Write `Primitive::Position` | **Sinks through the floor** (collision solver resolves the overlap) |
| 3 | `PlatformStand = true` + velocity | Ragdolls, still falls |
| 4 | Position + collisions disabled, self-integrated arc | Still broken |

**Key evidence:** walkspeed (a **Humanoid** field write) works. Everything that writes a
**Primitive** field fails. That points at either a wrong `Primitive::*` offset or an
invalid `hrp_primitive` pointer — *not* at the feature logic.

**Also confirmed:** `Humanoid::HumanoidRootPart (0x478)` reads back **`0x0`** — it is wrong
for this client build. The root part is currently resolved by **name** instead
(`"HumanoidRootPart"` among the character's children), which does work since ESP renders.

**⏭️ NEXT STEP — run this before writing any more code:**
Debug tab → **"test POSITION write (+10 studs up)"** and **"test VELOCITY write"**.
Each prints one line (also in the log tab). Interpretation:

- `hrp primitive: 0x0` → pointer resolution is broken; fix that first.
- `wpm=ok ... delta 0.00 ... did NOT stick` → **`Primitive::Position (0xec)` is the wrong
  offset** for this build. Re-dump it. This is the most likely outcome.
- `delta 10.00 ... LANDED` and you visibly teleport → writes are fine; the bug is in the
  feature logic and is then straightforward to fix.

### 4.2 ESP boxes sit slightly high
Most likely `Primitive::Size (0x1bc)` reads ~0, collapsing all 8 corners onto each part's
centre — the feet then contribute only a centre point, lifting the box bottom by half a foot.
A 1×1×1 fallback is in place, and the debug tab shows **`part size read`**. If that reads
zeros, `Primitive::Size` is wrong. If it reads real values (~`2.00, 2.00, 1.00`), look at
head/hat extent instead.

### 4.3 FOV changer
Was writing **degrees** into a field Roblox stores in **radians** — 70 became ~4010°,
which wrapped the projection and flipped the screen. Now converts and sanity-checks the
range. Click teleport had the same bug and was fixed too. Both unverified.

### 4.4 Skybox changer
It was doing: invalidate → write textures → set `SkyValid`/`LightingValid` back to **true**,
which tells the renderer its cached sky is still valid so it never re-uploads. Now it
invalidates *after* writing. Unverified. The world tab prints a live status string
(`skybox_debug_msg`) — read it: `No Sky in game`, `FAIL: RenderView invalid`,
`Skybox applied (...)` each point somewhere different.

---

## 5. Offsets — important

`offsets.h` matches the user-supplied dump for `version-f5a60436d48947d3`, verified
key-by-key (57 critical offsets: 0 mismatched, 0 missing).

**Known-wrong in this dump:**
- `Humanoid::HumanoidRootPart = 0x478` → reads `0x0`. Unused; name lookup is used instead.
- `Primitive::Position` / `Primitive::Size` → **suspect**, pending the write test.

**Entries the dump gave as `0x0`** (dumper couldn't resolve them). The previous known-good
values were kept rather than zeroing, and none are referenced in code:
`MouseService::SensitivityPointer`, `World::{worldStepsPerSec, FallenPartsDestroyHeight,
AirProperties, Primitives}`, `AirProperties::{AirDensity, GlobalWind}`,
`{Local,Module,}Script::ByteCode`.

### ⚠️ The `NameContainer` gotcha — read before updating offsets
The dump splits the instance name into **two** offsets:
```
Instance::NameContainer = 0x70
Instance::Name          = 0x8
```
`get_name()` must therefore do a **two-step** read (same pattern as `get_class_name`):
```cpp
uintptr_t container = read<uintptr_t>(address + Offsets::Instance::NameContainer);
return fetchstring(container + Offsets::Instance::Name);
```
Older dumps had a single absolute `Instance::Name = 0xb0`. Getting this wrong silently
returns **garbage names**, which breaks every name-based lookup — player detection, body
parts, skeletons, the root part — while class-name lookups keep working. That mismatch
caused a long stretch of "nothing works but walkspeed". Fixed in `8906f70`.

`offsets_dump.txt` (repo root) is intentionally **blank** — paste new dumps there.

---

## 6. Diagnostics built in

- **debug tab** — base address, VisualEngine, DataModel, game name, place id, game loaded,
  viewport, view matrix, local player, humanoid, hrp primitive, position/velocity/size
  read-backs, cached player count, and the two **write test** buttons.
- **log tab** — in-GUI log (replaces the old console), 200-line ring buffer, `LogLine(...)`.
- **world tab** — live skybox status string.
- "not in a game" warning when `place id == 0` (home page has no world/players).

---

## 7. Performance work already done

- **Zero-copy cache snapshots.** `GetEspEntities()` returned `EspEntity` **by value** from
  9 call sites/frame — ~1.4 MB of memcpy per frame at 30 players. Now
  `shared_ptr<const vector<...>>`.
- **`EspEntity` shrunk 5,320 → 2,760 bytes** (`part_names[64][64]` → `[64][24]`).
- **Batched child reads** — one `ReadProcessMemory` for the whole child array instead of
  one syscall per child.
- **Class-name cache** keyed by descriptor pointer (descriptors are stable, shared per class).
- **Single-pass traversal** — `fetch_entities` walked each child list 3× per player, now 1×.
- **`read_string_raw`** tries 128→64→32→16 bytes; a fixed 200-byte read fails entirely if
  it crosses into unmapped memory, which silently blanked names near page boundaries.
- **Release flags:** `/Ox`, Speed, AnySuitable inlining, omit frame pointers, string pooling,
  fast FP, AVX2, `/MP`, LTCG, `OptimizeReferences`, `COMDATFolding`.

> On "use a faster language": C++ is already correct here. The workload is **syscall-bound**
> (`ReadProcessMemory` ≈ 1–3µs each), not CPU-bound. Rust/C/asm would make identical
> syscalls at identical speed. Wins come from *fewer, larger* reads and less copying.

---

## 8. Malware audit (done at the start — clean)

No RAT/backdoor/stealer/dropper/persistence. Checked: outbound endpoints, HTTP verbs,
injection APIs, persistence, credential theft, encoded payloads, build-event hijacking,
binary artifacts. Only "risky-looking" APIs are inherent to the design:
`WriteProcessMemory` into Roblox, and `ShellExecuteA` to open the config folder.

Since the mesh backend was deleted, the project makes **no outbound network requests at all**.

---

## 9. Housekeeping / possible next cleanups

- Rename namespace `discord_overlay` → something accurate (cosmetic, zero risk).
- `overlay.h` is a 6-line shim over `overlay.hpp` — could be folded in.
- `.vcxproj.filters` isn't maintained (VS shows a flat tree).
- Nothing in the repo is compiled but unused any more (verified after the mesh removal).

## 10. Changelog (this branch)

```
79a5dba Remove orphaned mesh cham backend and all references
c24c828 Revert mesh backend enablement (headers incompatible), drop /GS- sdl conflict
2a24d66 Enable mesh cham implementations, menu opens on start, accent on separators, part-size guard
a4fd54c Zero-copy cache snapshots, shrink EspEntity, full release optimizations, fix write test target
ecafe69 Even nav grid, transparency across all backgrounds, add position write test
a008b4b Fix FOV radians bug, rewrite flight + infinite jump, live accent, uniform UI metrics
bf15a63 Condense tabs, UI config page + FOV changer, taskbar window, HRP via humanoid, skybox invalidation
0d07619 PHETAMINE-style UI, tray icon + in-app exit, toggle flight, impulse infinite jump
544490a Add infinite jump + inventory checker, remove korblox, click teleport client coords
6ae48aa Position-based flight, click teleport 3rd person, modern GUI, remove 3d esp preview, consolidate keybinds
2d985a2 Remove console for in-gui log, keybinds tab, optimize memory reads
b22c49a Fix ESP box rotation accuracy, improve flight, add click teleport
8906f70 Fix Instance name reads (NameContainer), auto-reconnect, click-through outside menu
a5c179a Restore focus to Roblox when menu closes, flag not-in-game state
dfff981 Remove game-specific features, glassy red theme, debug tab
a18a7f1 Replace Discord overlay hook with standalone transparent overlay window
bf8e898 Change menu toggle key from INSERT to HOME
015cc98 Update offsets to client version-f5a60436d48947d3
```

---

## 11. TL;DR for whoever picks this up

1. **Run the two write tests in the debug tab.** Flight, infinite jump and click teleport
   are all blocked on that one measurement. Don't write more movement code before you have it.
2. If the position write doesn't stick → re-dump `Primitive::Position` and `Primitive::Size`.
3. Check `part size read` in the same tab to settle the ESP box offset.
4. Read the skybox status string in the world tab before touching that feature.
5. When updating offsets, **mind the `NameContainer` two-step read** (§5).
