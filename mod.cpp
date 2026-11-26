#include <mod/amlmod.h>
#include <mod/logger.h>
#include <mod/config.h>
#include <stdio.h>

MYMODCFG(net.fpsmod.teste, FPS Teste, 13.0, VoceMesmo)

// v13: so grava enderecos em fpsteste.txt. Nao altera nada no jogo.
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
    if (!lib || !h) { Note("v13: libGTASA nao encontrada"); return; }

    static const char* names[] = {
        "_ZN4CPed9PreRenderEv",
        "_ZN4CPed6RenderEv",
        "_ZN4CPed17PreRenderAfterTestEv",
        "_ZN10CPlayerPed9PreRenderEv",
        "_ZN10CPlayerPed6RenderEv",
        "_ZTV10CPlayerPed",
        "_Z29GetAnimHierarchyFromSkinClumpP7RpClump",
        "_Z17RpHAnimIDGetIndexP16RpHAnimHierarchyi",
        "RpHAnimIDGetIndex",
        "_Z32RpHAnimHierarchyGetMatrixArrayP16RpHAnimHierarchy",
        "RpHAnimHierarchyGetMatrixArray",
        "RwMatrixScale",
        "_Z13RwMatrixScaleP11RwMatrixTagPK5RwV3d15RwOpCombineType",
        "TheCamera",
    };
    const unsigned count = sizeof(names) / sizeof(names[0]);
    uintptr_t addr[count];
    char b[220];

    Note("v13: enderecos");
    for (unsigned i = 0; i < count; i++)
    {
        addr[i] = (uintptr_t)aml->GetSym(h, names[i]);
        if (addr[i]) snprintf(b, sizeof(b), "%s = 0x%x", names[i], (unsigned)(addr[i] - lib));
        else snprintf(b, sizeof(b), "%s = NAO ENCONTRADO", names[i]);
        Note(b);
    }

    // em que posicao da tabela do jogador estao as funcoes de desenho (so leitura)
    uintptr_t vt = addr[5];
    if (vt)
    {
        for (int slot = 0; slot < 40; slot++)
        {
            uintptr_t v = *(uintptr_t*)(vt + 8 + slot * 4);
            snprintf(b, sizeof(b), "tabela[%d] slot=0x%x valor=0x%x", slot,
                     (unsigned)(vt + 8 + slot * 4 - lib), (unsigned)(v - lib));
            Note(b);
        }
    }
    Note("v13: fim");
    aml->ShowToast(true, "FPS Teste v13: enderecos gravados");
}
