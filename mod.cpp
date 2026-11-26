#include <mod/amlmod.h>
#include <mod/logger.h>
#include <mod/config.h>
#include <stdio.h>

MYMODCFG(net.fpsmod.teste, FPS Teste, 12.0, VoceMesmo)

// v12: so grava enderecos em fpsteste.txt. Nao altera nada no jogo.
static void Note(const char* msg)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/fpsteste.txt", aml->GetAndroidDataPath());
    FILE* f = fopen(path, "a");
    if (f) { fprintf(f, "%s\n", msg); fclose(f); }
}

extern "C" void OnModLoad()
{
    logger->SetTag("FPS Teste");
    uintptr_t lib = aml->GetLib("libGTASA.so");
    void* h = aml->GetLibHandle("libGTASA.so");
    if (!lib || !h) { Note("v12: libGTASA nao encontrada"); return; }

    static const char* names[] = {
        "_ZN4CPed15GetBonePositionER5RwV3djb",
        "_ZN4CPed26GetTransformedBonePositionER5RwV3djb",
        "_ZN6CPools6GetPedEi",
        "_ZN6CPools9GetPedRefEP4CPed",
        "_Z13FindPlayerPedi",
    };

    Note("v12: enderecos");
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++)
    {
        uintptr_t a = (uintptr_t)aml->GetSym(h, names[i]);
        char b[200];
        if (a) snprintf(b, sizeof(b), "%s = 0x%x", names[i], (unsigned)(a - lib));
        else snprintf(b, sizeof(b), "%s = NAO ENCONTRADO", names[i]);
        Note(b);
    }
    Note("v12: fim");
    aml->ShowToast(true, "FPS Teste v12: enderecos gravados");
}
