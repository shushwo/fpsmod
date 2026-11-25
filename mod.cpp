#include <mod/amlmod.h>
#include <mod/logger.h>
#include <mod/config.h>
#include <time.h>
#include <math.h>

MYMODCFG(net.fpsmod.teste, FPS Teste, 9.0, VoceMesmo)

static uintptr_t gLib = 0;
typedef void* (*GetTaskUseGun_t)(void* intelligence);
typedef void* (*FindPlayerPed_t)(int);
static GetTaskUseGun_t GetTaskUseGun = NULL;
static FindPlayerPed_t FindPlayerPed = NULL;

static void Note(const char* msg)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/fpsteste.txt", aml->GetAndroidDataPath());
    FILE* f = fopen(path, "a");
    if (f) { fprintf(f, "%ld %s\n", (long)time(NULL), msg); fclose(f); }
}

struct Vec2 { float x, y; };
static float gStickX = 0, gStickY = 0;
static bool gForce = false;
static int gLogs = 0;

static bool HoldingFirearm()
{
    if (!FindPlayerPed) return false;
    char* ped = (char*)FindPlayerPed(-1);
    if (!ped) return false;
    signed char slot = *(signed char*)(ped + 0x71c);
    if (slot < 0 || slot > 12) return false;
    int type = *(int*)(ped + 0x5a4 + slot * 0x1c);
    return type >= 22 && type <= 34;
}

// bool CTouchInterface::IsTouched(WidgetIDs, CVector2D*, int)   (via GOT)
typedef bool (*IsTouched_t)(int, Vec2*, int);
static IsTouched_t IsTouched_orig = NULL;
static bool HookOf_IsTouched(int id, Vec2* v, int n)
{
    if (id == 0xa7)
    {
        bool r = IsTouched_orig(id, v, n);
        if (r && v) { gStickX = v->x; gStickY = v->y; }
        else { gStickX = 0; gStickY = 0; }
        // decide se forca a mira: andando de lado com arma de fogo
        bool want = (fabsf(gStickX) > 0.6f) && HoldingFirearm();
        if (want != gForce)
        {
            gForce = want;
            if (gLogs < 150)
            {
                gLogs++;
                char b[100];
                snprintf(b, sizeof(b), "FORCA %s x=%.2f y=%.2f", want ? "LIGADA" : "desligada", gStickX, gStickY);
                Note(b);
            }
        }
        return r;
    }
    if (id == 0x13 && gForce) return true;
    return IsTouched_orig(id, v, n);
}

// bool CTaskSimplePlayerOnFoot::ProcessPed(CPed*)  (via vtable) - so registra o estado
typedef bool (*ProcessPed_t)(void*, void*);
static ProcessPed_t ProcessPed_orig = NULL;
static int lastState = -1;
static bool HookOf_ProcessPed(void* self, void* ped)
{
    if (GetTaskUseGun && ped)
    {
        void* intel = *(void**)((char*)ped + 0x440);
        int state = (intel && GetTaskUseGun(intel)) ? 1 : 0;
        if (state != lastState && gLogs < 150)
        {
            lastState = state;
            gLogs++;
            Note(state ? "MIRANDO (tarefa de arma ativa)" : "ANDANDO (sem tarefa de arma)");
        }
    }
    return ProcessPed_orig(self, ped);
}

static bool PatchGot(uintptr_t gotOff, uintptr_t expectedFn, void* hook, void** orig)
{
    uintptr_t* got = (uintptr_t*)(gLib + gotOff);
    if (*got != gLib + expectedFn) return false;
    *orig = (void*)*got;
    aml->Unprot((uintptr_t)got, 4);
    *got = (uintptr_t)hook;
    return true;
}

extern "C" void OnModLoad()
{
    logger->SetTag("FPS Teste");
    uintptr_t lib = aml->GetLib("libGTASA.so");
    if (!lib) return;
    gLib = lib;
    void* h = aml->GetLibHandle("libGTASA.so");
    GetTaskUseGun = (GetTaskUseGun_t)aml->GetSym(h, "_ZNK16CPedIntelligence13GetTaskUseGunEv");
    FindPlayerPed = (FindPlayerPed_t)aml->GetSym(h, "_Z13FindPlayerPedi");

    bool ok = false;
    uintptr_t vt = lib + 0x66cf30;
    for (int i = 0; i < 13; i++)
    {
        uintptr_t* slot = (uintptr_t*)(vt + i * 4);
        if (*slot == lib + 0x539f2d)
        {
            ProcessPed_orig = (ProcessPed_t)(*slot);
            aml->Unprot((uintptr_t)slot, 4);
            *slot = (uintptr_t)&HookOf_ProcessPed;
            ok = true;
            break;
        }
    }
    bool ok2 = PatchGot(0x674388, 0x2b0cbd, (void*)&HookOf_IsTouched, (void**)&IsTouched_orig);
    Note(ok2 && FindPlayerPed ? "v9 carregado: ganchos OK" : "v9 carregado: ALGUM GANCHO FALHOU");
    aml->ShowToast(true, ok && ok2 ? "FPS Teste v9: ganchos OK" : "FPS Teste v9: gancho FALHOU");
}
