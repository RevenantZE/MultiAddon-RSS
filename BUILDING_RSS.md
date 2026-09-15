# Reproducing the Linux SteamRT3 build

Use a clean checkout of the release source commit, Git, Docker Linux containers,
and the dependency pins in dependencies.lock.json. Output must be outside source
and dependencies. Use a new output directory for each source commit.

```bash
git clone https://github.com/alliedmodders/metamod-source deps/mmsource-api17
git -C deps/mmsource-api17 checkout 26c03fa192c3662134edd71dffd757c7accae7a2
git -C deps/mmsource-api17 submodule update --init --recursive
git clone https://github.com/alliedmodders/hl2sdk deps/hl2sdk-cs2
git -C deps/hl2sdk-cs2 checkout bd17582be4bc18970e4fe4c518359872fda8276d
git clone https://github.com/alliedmodders/hl2sdk-manifests deps/hl2sdk-manifests
git -C deps/hl2sdk-manifests checkout 25562f7019dbe9534a61a5e8b1959ea4e36a6e9e
bash scripts/build-steamrt3.sh "$PWD/deps" "$PWD/../mam-build"
```

This branch targets Metamod API17 using real API17 headers. KHook and SafetyHook
are vendored and statically linked privately; see third_party/khook/RSS_STANDALONE.md.
The api18_khook branch instead uses the API18 core's KHook service.

The script verifies clean dependency commits, mounts source/dependencies
read-only, runs helper tests with C++17 / no exceptions / no RTTI, configures an
optimized x86-64 build, then runs ambuild serially twice. Failure returns nonzero.
Each command prints its actual EXIT marker. The image digest pins the toolchain;
byte-for-byte reproducibility across filesystem paths/times is not claimed.

Artifacts are under package/. Keep operational config: the included cfg is a
neutral example, not an RSS server backup. Live server load, clients joining,
addon staging/mounting, map changes, OFF/ON toggles and failure recovery require
testing on an authorized CS2 server. Do not hot-reload as a substitute.
Prior 18e8b90 binaries are not current release evidence.

Release source is the tagged commit. dependencies.lock.json identifies external
build source inputs; preserve their licenses. The release manifest links the
newly produced .so SHA256 to that source commit.
