# RSS API17 standalone integration

Imported from alliedmodders/khook at
`8f6430fd5ed91de4c701a7e395f79da0c14c14ec` (zlib license).
SafetyHook is imported from alliedmodders/safetyhook at
`ec3f698a1d9936d72c57c639536fbbedab6d7c8a` (BSL-1.0).
Original license files and source authorship are preserved.
Only the library sources, headers and AMBuilder files are vendored.

RSS changes to detour.cpp: lazy worker startup, atomic termination flag,
idempotent shutdown with synchronous wrapper-ID retirement, and a same-file
destructor guard. Plugin Unload calls Shutdown after detaching its callbacks.
The Virtual removal callback also erases its forward hook-ID entry so its
destructor cannot remove an already-retired hook after runtime teardown.
Virtual detours retain their original vtable entry and restore it before
freeing the JIT. Teardown must run while the engine vtable is alive and no
third-party detour is chained through this private JIT. An inability to make
an owned vtable entry writable aborts instead of leaving a dangling jump.
Normal plugin Unload preflights ownership/protection before changing any
plugin state and refuses unload if a foreign vtable chain is present.
Shutdown is a quiescent lifecycle operation, not safe inside an active hook
or concurrently with new hook registration.

API17 uses the real API17 Metamod headers and links both libraries privately.
It does not relabel an API18 interface. Linker archive hiding is restricted to
these two archives so the plugin factory remains exported. The api18_khook
branch uses the core KHook service and does not build this copy.
Do not use blanket Bsymbolic-functions: it changes protobuf bindings against
tier0 as well as KHook. The two private archives alone are hidden.
