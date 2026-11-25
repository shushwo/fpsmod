#include <mod/amlmod.h>
#include <mod/logger.h>
#include <mod/config.h>
#include <time.h>

MYMODCFG(net.fpsmod.teste, FPS Teste, 2.0, VoceMesmo)

typedef void* (*GetTaskUseGun_t)(void* intelligence);
static GetTaskUseGun_t GetTaskUseGun = NULL;
static uintptr_t gLib = 0;
static void Note(const char* msg);
static void DumpPad(const char* tag, void* ped)
{
    char buf[900]; int n = 0;
    unsigned char* p = (unsigned char*)(gLib + 0x959b1c);
    n += snprintf(buf + n, sizeof(buf) - n, "%s PAD:", tag);
    for (int i = 0; i < 0x60; i++) n += snprintf(buf + n, sizeof(buf) - n, "%02x", p[i]);
    signed char slot = *(signed char*)((char*)ped + 0x71c);
    int wtype = *(int*)((char*)ped + 0x5a4 + slot * 0x1c);
    n += snprintf(buf + n, sizeof(buf) - n, " slot=%d arma=%d", (int)slot, wtype);
    Note(buf);
}
static int lastState = -1;
static time_t lastToast = 0;
static void Note(const char* msg)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/fpsteste.txt", aml->GetAndroidDataPath());
    FILE* f = fopen(path, "a");
    if (f) { fprintf(f, "%ld %s\n", (long)time(NULL), msg); fclose(f); }
}

// CHID::IsPressed(HIDMapping, float*) - argumentos lidos como inteiros crus (sem desreferenciar)
struct HidEnt { uint32_t a, b; unsigned char last; bool used; };
static HidEnt hidTab[64];
static int hidLogs = 0;
static int hidCalls = 0;
DECL_HOOKb(HID_IsPressed, uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d)
{
    bool r = HID_IsPressed(a, b, c, d);
    if (++hidCalls == 1000) Note("HID: 1000 chamadas");
    if (hidLogs < 300)
    {
        int idx = -1;
        for (int i = 0; i < 64; i++)
            if (hidTab[i].used && hidTab[i].a == (uint32_t)a && hidTab[i].b == (uint32_t)b) { idx = i; break; }
        bool isNew = false;
        if (idx < 0)
            for (int i = 0; i < 64; i++)
                if (!hidTab[i].used) { hidTab[i].used = true; hidTab[i].a = (uint32_t)a; hidTab[i].b = (uint32_t)b; hidTab[i].last = 2; idx = i; isNew = true; break; }
        if (idx >= 0 && hidTab[idx].last != (unsigned char)r)
        {
            hidTab[idx].last = (unsigned char)r;
            hidLogs++;
            char buf[120];
            snprintf(buf, sizeof(buf), "HID %s a=%u b=%u c=%u -> %s", isNew ? "novo" : "mudou", (unsigned)a, (unsigned)b, (unsigned)c, r ? "APERTADO" : "solto");
            Note(buf);
        }
    }
    return r;
}

// bool CTaskSimplePlayerOnFoot::ProcessPed(CPed*)
typedef bool (*ProcessPed_t)(void*, void*);
static ProcessPed_t PlayerOnFoot_ProcessPed = NULL;
static bool HookOf_PlayerOnFoot_ProcessPed(void* self, void* ped)
{
    static int calls = 0;
    if (++calls == 300) Note("gancho esta sendo chamado");
    if (GetTaskUseGun && ped)
    {
        void* intel = *(void**)((char*)ped + 0x440);
        int state = (intel && GetTaskUseGun(intel)) ? 1 : 0;
        time_t now = time(NULL);
        if (state != lastState && now - lastToast >= 2)
        {
            lastState = state;
            lastToast = now;
            Note(state ? "MIRANDO (tarefa de arma ativa)" : "ANDANDO (sem tarefa de arma)");
            DumpPad(state ? "mirando" : "andando", ped);
        }
    }
    return PlayerOnFoot_ProcessPed(self, ped);
}

extern "C" void OnModLoad()
{
    logger->SetTag("FPS Teste");
    uintptr_t lib = aml->GetLib("libGTASA.so");
    gLib = lib;
    if (!lib) { logger->Error("sem libGTASA"); return; }
    void* h = aml->GetLibHandle("libGTASA.so");
    GetTaskUseGun = (GetTaskUseGun_t)aml->GetSym(h, "_ZNK16CPedIntelligence13GetTaskUseGunEv");
    bool ok = false;
    uintptr_t target = lib + 0x539f2d;
    uintptr_t vt = lib + 0x66cf30;
    for (int i = 0; i < 13; i++)
    {
        uintptr_t* slot = (uintptr_t*)(vt + i * 4);
        if (*slot == target)
        {
            PlayerOnFoot_ProcessPed = (ProcessPed_t)(*slot);
            aml->Unprot((uintptr_t)slot, 4);
            *slot = (uintptr_t)&HookOf_PlayerOnFoot_ProcessPed;
            ok = true;
            break;
        }
    }
    bool ok2 = false;
    {
        uintptr_t* got = (uintptr_t*)(lib + 0x672774);
        if (*got == lib + 0x28c181)
        {
            HID_IsPressed = (bool (*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t))(*got);
            aml->Unprot((uintptr_t)got, 4);
            *got = (uintptr_t)&HookOf_HID_IsPressed;
            ok2 = true;
        }
    }
    Note(ok2 ? "HID gancho OK" : "HID gancho FALHOU");
    Note(ok ? "v8c carregado: gancho OK" : "v8c carregado: gancho FALHOU");
    aml->ShowToast(true, ok ? "FPS Teste v8c carregado: gancho OK" : "FPS Teste v8c carregado: gancho FALHOU");
    logger->Info("Gancho instalado");
}
