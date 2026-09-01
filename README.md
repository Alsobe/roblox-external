# roblox external

usermode external for roblox. 
reads game memory from outside the process, renders esp / aimbot / chams / misc features in its own standalone imgui overlay window.

## features

- esp (box, skeleton, name, distance, health, tool, rig type)
- aimbot (camera + mouse modes, smoothing, fov, prediction, sticky aim)
- chams (basic + mesh chams with union clipping)
- expanded hitbox
- china hat esp
- aim viewer
- noclip
- walkspeed
- flight
- customizable keybinds for aimbot, noclip, walkspeed and flight!

## setup

- visual studio 2022+ (v145 toolset)
- c++20

### dependencies

all included in the repo:

- [imgui](https://github.com/ocornut/imgui) - rendering + ui
- [clipper2](https://github.com/AngusJohnson/Clipper2) - used for mesh chams union clipping

### building

1. open `roblox external.slnx` in visual studio
2. build x64 release
3. run the exe as administrator

## how it works

the overlay creates its own layered, click-through, always-on-top window and renders with dx11 imgui. no discord required. memory is read externally via `ReadProcessMemory` — nothing is injected into the roblox process.

features run in a separate thread, esp/rendering happens on the overlay thread.

## keys

- `HOME` toggles the menu (change `TOGGLE_KEY` in `overlay.hpp` to rebind)
- some feature have their own configurable keybind in the menu

## notes

- run roblox in windowed or borderless mode - overlays cannot draw over exclusive fullscreen
- run the exe as administrator so it can open a handle to roblox
- this targets a specific roblox client version — offsets need to be updated if the version changes

showcase:
![UI Showcase](https://i.imgur.com/NNNYAUD.png)
![UI Showcase](https://i.imgur.com/9vwzf9l.png)