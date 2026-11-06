# 1.4.3 / build 105: experimental source port

This branch adapts the upstream 1.4.2 Windows recompiler to the owner's macOS
1.4.3 build 105. It requires Unity **2020.3.49f1** and the exact input hashes in
`tools/port143/profile.json`. Do not disable input verification for another
version. Original game files are never modified.

## What is published

Handwritten runtime adapters, translator changes, RVA configuration, shader
layout fixes, private build/staging tools, and a Windows isolation launcher.
The MIT license and upstream attribution are retained. Unity and other
dependencies retain their own terms; see `THIRD_PARTY_NOTICES.md`.

No game application, assets, audio, text, save, App Store receipt, metadata
dump, translated game C++, Unity SDK, or game-containing DLL is included.
Build these privately using your own legitimate copy. Do not attach them to
issues, commits, CI artifacts or releases. No binary release is provided by
this branch.

## Status and limits

- Known runtime target coverage: 722/722; 115 native adapters and 348 translated
  helper functions. This is a static inventory, not proof that all indirect
  calls or all game features work.
- User playtesting reached quest 2-3, including Maria, without a reported crash.
- Save/restart was checked with the 2-2 checkpoint in a network-disabled Sandbox.
- Native Windows testing loaded a copied 2-3 checkpoint (rank 5, 5.1%), opened
  home/menu, wrote to an isolated save directory and exited with code 0.
  Native Windows combat and a full campaign have not been exhaustively tested.
- Coin rendering uses a scoped culling workaround for drop/homing textures;
  effects sharing those textures may also be affected. This is not a general
  GL/D3D winding correction.
- The legacy progress-changing debug menu is excluded from the 1.4.3 build.

## Private build

Use Linux/WSL, LLVM/clang/lld 18, xwin CRT/Windows SDK, Python with LIEF,
Capstone, UnityPy and lz4, Il2CppDumper, and your installed Unity 2020.3.49f1
Editor plus Windows IL2CPP support. Keep the build directory **outside this
checkout**. The upstream `BUILDING.md` explains SDK/header case aliases and
glslang/SPIRV-Cross dependencies. Substitute 2020.3.49f1 throughout.

1. Expand your own unmodified macOS app locally. ZIP is acceptable; renaming
   ZIP to DMG is unnecessary. Verify the archive first using your own hash.
2. Extract only the verified arm64 inputs and metadata:

   ```sh
   python tools/port143/prepare_inputs.py --contents /path/to/App.app/Contents --out /private/build/inputs
   ```

   This validates version/build and hashes, and never reads the receipt.
3. Run Il2CppDumper locally on the arm64 game image and metadata, producing
   `/private/build/dump/script.json` and `il2cpp.h`. Do not commit its output.
4. Regenerate private tables, ABI bridges and translated C++:

   ```sh
   python tools/port143/generate.py --binary /private/build/inputs/GameAssembly_arm64.dylib --dump /private/build/dump --work /private/build
   ```

   `--analyze-only` checks tables without translating. The expected 291804
   main functions and 348 helper functions were reproduced from the verified
   input. Full compilation requires substantial RAM and time. Generated code
   uses `/Od` by default in the portable build to avoid pathological optimizer
   runs; faster configurations need their own validation.
5. Set `UNITY_IL2CPP`, `UNITY_WIN64`, `MSVC_SYSROOT`, `CASE_ALIASES`, and
   `THIRD_PARTY` as described upstream, then build:

   ```sh
   export RECOMP_OUT=/private/build/generated GAP_OUT=/private/build/gap
   export BRIDGES_OUT=/private/build/bridges BUILD_DIR=/private/build/objects
   bash recomp/winbuild/build_gameassembly_143.sh
   bash plugins/apple/build.sh /private/build/apple-plugins
   bash plugins/spfx/build_143.sh /private/build/inputs/SPFXUnityPlugin_arm64.macho /private/build/spfx
   bash tools/windows143/build.sh /private/build/windows-tools
   ```

   Portable orchestration remains experimental. Do not assume an unrelated
   SDK or input version is compatible because compilation succeeds.
6. Build `tools/shaders/hlslc.cpp` using the upstream shader instructions
   (Windows or Wine and d3dcompiler_47 are required). Convert local asset copies:

   ```sh
   python tools/port143/convert_assets.py --data /path/to/App.app/Contents/Resources/Data --hlslc /private/hlslc --work /private/shader-work --out /private/converted
   ```

7. Assemble a fresh private installation with `tools/port143/stage.py --help`.
   Supply local Data/converted files, verified inputs, compiled GameAssembly
   and SPFX DLLs, offline Apple plugins, Windows player files and the two
   launcher tools. `--save-source` optionally copies an owner's existing save.
   The staging tool refuses a non-empty output. Do not upload the installation.

## Normal Windows launch with communication blocked

Run `Setup-Firewall.ps1 -HostRoot <installation>` once with administrator
approval. It adds only per-program inbound/outbound **Block** rules for the
game and its launcher/probe, all profiles/protocols/remote addresses. It does
not enable/disable network adapters or change global firewall defaults.

Start `Play.cmd`. Before the game starts, the launcher:

1. Checks every manifest hash and effective firewall rules for its current
   executable paths. Moving the installation requires configuring new rules.
2. Runs a TEST-NET-1 TCP probe and requires Windows access-denied error 10013.
3. Creates the game suspended, applies a one-process Job Object and Windows
   child-process restriction, then resumes it. Child creation is blocked;
   external crash helpers are therefore unavailable in this mode.
4. Sets `GOSR_SAVE_ROOT` to `saves/quest2-3` in this private installation.
   The normal Unity LocalLow save directory is not used. Keep backups.
5. Preserves the tested SPFX worker and coin-rendering settings.

Any failed check prevents launch. Do not launch `GoS.exe` directly; that skips
the checks and save override. There is no playtime timeout. Closing the
launcher while the game runs terminates its Job Object, so close the game
normally first. Local proof/log files remain under `output/`.

Apple plugin calls return offline/unavailable/no-account responses. They do
not authenticate to Apple or upload saves. These replacements alone are not
an OS network barrier; keep the firewall and process restrictions enabled.
No Apple ID, Arcade, App Store, iCloud or Game Center login is required.

To remove this installation's rules, run `Setup-Firewall.ps1 -HostRoot
<installation> -Remove` with administrator approval after exiting the game.
Only the matching per-program rules in this task's group are removed.

## Publication check

Stage explicit source/documentation paths, then run:

```sh
python tools/port143/check_publication.py
git diff --cached --stat
```

The index check rejects binary/private paths, translated game bodies,
credential patterns and personal absolute build paths. Manually review the
diff too. Never use a game installation or build directory as the Git root.
