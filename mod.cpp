#include <mod/amlmod.h>
#include <mod/logger.h>
#include <mod/config.h>
#include <time.h>

MYMODCFG(net.fpsmod.teste, FPS Teste, 2.0, VoceMesmo)

typedef void* (*GetTaskUseGun_t)(void* intelligence);
static GetTaskUseGun_t GetTaskUseGun = NULL;
static int lastState = -1;
static time_t lastToast = 0;

// bool CTaskSimplePlayerOnFoot::ProcessPed(CPed*)
DECL_HOOKb(PlayerOnFoot_ProcessPed, void* self, void* ped)
{
    static int calls = 0;
    if (++calls == 300) aml->ShowToast(false, "FPS Teste: gancho esta sendo chamado");
    if (GetTaskUseGun && ped)
    {
        void* intel = *(void**)((char*)ped + 0x440);
        int state = (intel && GetTaskUseGun(intel)) ? 1 : 0;
        time_t now = time(NULL);
        if (state != lastState && now - lastToast >= 2)
        {
            lastState = state;
            lastToast = now;
            aml->ShowToast(false, state ? "FPS Teste: MIRANDO (tarefa de arma ativa)" : "FPS Teste: ANDANDO (sem tarefa de arma)");
        }
    }
    return PlayerOnFoot_ProcessPed(self, ped);
}

extern "C" void OnModLoad()
{
    logger->SetTag("FPS Teste");
    uintptr_t lib = aml->GetLib("libGTASA.so");
    if (!lib) { logger->Error("sem libGTASA"); return; }
    void* h = aml->GetLibHandle("libGTASA.so");
    GetTaskUseGun = (GetTaskUseGun_t)aml->GetSym(h, "_ZNK16CPedIntelligence13GetTaskUseGunEv");
    bool ok = HOOKSYM(PlayerOnFoot_ProcessPed, h, "_ZN23CTaskSimplePlayerOnFoot10ProcessPedEP4CPed");
    aml->ShowToast(true, ok ? "FPS Teste v2 carregado: gancho OK" : "FPS Teste v2 carregado: gancho FALHOU");
    logger->Info("Gancho instalado");
}
