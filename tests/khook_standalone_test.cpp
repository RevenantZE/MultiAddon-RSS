#include <cstdio>
#include <khook.hpp>
#include <khook/memory.hpp>

class Probe {
public:
    virtual int Read(int value) { return value + 1; }
};

static int calls = 0;
static KHook::Return<int> Before(Probe*, int) {
    ++calls;
    return {KHook::Action::Ignore, 0};
}
__attribute__((noinline)) static int Invoke(Probe* probe, int value) {
    return probe->Read(value);
}
static int Foreign(Probe*, int value) { return value + 99; }

// Keep the target away from this harness's PLT: SafetyHook temporarily traps
// its target page. In production the target lives in a separate engine DSO.
__attribute__((noinline, aligned(4096))) static int FunctionTarget(int value) {
    volatile int result = value + 3;
    return result;
}
static KHook::Return<int> FunctionBefore(int) {
    ++calls;
    return {KHook::Action::Ignore, 0};
}

int main() {
    KHook::Shutdown();
    KHook::Shutdown();
    Probe probe;
    void** table = *reinterpret_cast<void***>(&probe);
    void* original = table[0];
    {
        KHook::Virtual<Probe, int, int> hook(&Probe::Read, Before, nullptr);
        hook.Add(&probe);
        if (table[0] == original || Invoke(&probe, 5) != 6 || calls != 1) return 1;
        if (!KHook::CanShutdown()) return 2;

        // A foreign chain must prevent normal plugin unload.
        void* owned = table[0];
        if (!KHook::Memory::SetAccess(table, sizeof(void*), 7)) return 3;
        table[0] = reinterpret_cast<void*>(&Foreign);
        if (KHook::CanShutdown()) return 4;
        table[0] = owned;
        if (!KHook::Memory::SetAccess(table, sizeof(void*), 5)) return 5;

        KHook::Shutdown();
        if (table[0] != original || Invoke(&probe, 5) != 6 || calls != 1) return 6;
        KHook::Shutdown();
        // The wrapper stays alive across shutdown, then can bind anew.
        hook.Add(&probe);
        if (table[0] == original || Invoke(&probe, 5) != 6 || calls != 2) return 7;
        KHook::Shutdown();
        if (table[0] != original) return 8;
    }
    {
        KHook::Function<int, int> hook(FunctionTarget, FunctionBefore, nullptr);
        int (*volatile invoke)(int) = FunctionTarget;
        if (invoke(5) != 8 || calls != 3) return 9;
        KHook::Shutdown();
        if (invoke(5) != 8 || calls != 3) return 10;
    }
    KHook::Shutdown();
    std::puts("STANDALONE_HOOK_PASS");
    return 0;
}
