#include <mod/amlmod.h>
#include <mod/logger.h>
#include <mod/config.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

MYMODCFG(net.fpsmod.teste, FPS Teste, 21.0, VoceMesmo)

// v21: esconde cabeca e tronco do CJ SO quando ele esta em moto/bicicleta/quadri
// e a camera esta perto da cabeca (primeira pessoa). Nao altera arquivos.
static void Note(const char* msg)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/fpsteste.txt", aml->GetAndroidDataPath());
    FILE* f = fopen(path, "a");
    if (f) { fprintf(f, "%s\n", msg); fclose(f); }
}

typedef void* (*FindPlayerPed_t)(int);
typedef void* (*FindPlayerVehicle_t)(int, bool);
typedef void* (*GetHier_t)(void*);
typedef int   (*GetIndex_t)(void*, int);
typedef void  (*Render_t)(void*);

static FindPlayerPed_t     FindPlayerPed;
static FindPlayerVehicle_t FindPlayerVehicle;
static GetHier_t           GetHier;
static GetIndex_t          GetIndex;
static Render_t            Render_orig;
static char*               Camera;

struct Matrix { float right[3]; uint32_t flags; float up[3]; uint32_t p1;
                float at[3]; uint32_t p2; float pos[3]; uint32_t p3; };

static bool IsBikeModel(int m)
{
    static const int bikes[] = { 448,461,462,463,468,471,481,509,510,521,522,523,581,586 };
    for (unsigned i = 0; i < sizeof(bikes) / sizeof(bikes[0]); i++)
        if (bikes[i] == m) return true;
    return false;
}

static int gLastModel = -1;
static bool gLastHide = false;
static int gLogs = 0;

// destino especial: ponto atras da camera, no lado oposto aos cotovelos
static const int BAIXO = -100;

static void HookOf_Render(void* ped)
{
    if (ped != FindPlayerPed(-1)) { Render_orig(ped); return; }

    void* veh = FindPlayerVehicle(-1, false);
    int model = veh ? *(uint16_t*)((char*)veh + 0x26) : 0;

    void* clump = *(void**)((char*)ped + 0x18);
    void* hier = (veh && clump) ? GetHier(clump) : NULL;
    Matrix* array = hier ? *(Matrix**)((char*)hier + 8) : NULL;
    int nodes = hier ? *(int*)((char*)hier + 4) : 0;
    int head = array ? GetIndex(hier, 5) : -1;
    char* camMatrix = *(char**)(Camera + 0x14);

    bool hide = false;
    float dist2 = -1.0f;
    if (head >= 0 && head < nodes && nodes < 100 && camMatrix)
    {
        float* c = (float*)(camMatrix + 0x30);
        float dx = array[head].pos[0] - c[0];
        float dy = array[head].pos[1] - c[1];
        float dz = array[head].pos[2] - c[2];
        dist2 = dx * dx + dy * dy + dz * dz;
        hide = IsBikeModel(model) && dist2 < 1.2f * 1.2f;
    }

    if ((model != gLastModel || hide != gLastHide) && gLogs < 200)
    {
        gLastModel = model; gLastHide = hide; gLogs++;
        char b[160];
        snprintf(b, sizeof(b), "veiculo=%d cabeca=%d ossos=%d dist2=%.2f esconder=%s",
                 model, head, nodes, dist2, hide ? "sim" : "nao");
        Note(b);
    }

    if (!hide) { Render_orig(ped); return; }

    // encolhe cabeca e tronco, desenha e devolve. Bracos, maos e pernas ficam.
    // {osso a encolher, osso para onde ele vai}
    static const int shrink[][2] = {
        {5,BAIXO}, {6,BAIXO}, {7,BAIXO}, {8,BAIXO},   // cabeca, sobrancelhas, queixo
        {2,BAIXO}, {3,BAIXO}, {4,BAIXO},              // coluna, peito, pescoco
        {301,BAIXO}, {302,BAIXO},                     // peitorais
        {21,23}, {22,23},     // ombro e braco esquerdo -> cotovelo esquerdo
        {31,33}, {32,33},     // ombro e braco direito -> cotovelo direito
    };
    const int N = sizeof(shrink) / sizeof(shrink[0]);
    Matrix saved[N];
    int idx[N];
    float point[N][3];
    int le = GetIndex(hier, 23), re = GetIndex(hier, 33);
    if (le < 0 || le >= nodes || re < 0 || re >= nodes) { Render_orig(ped); return; }
    float low[3];
    float* cam = (float*)(camMatrix + 0x30);
    for (int k = 0; k < 3; k++)
        low[k] = cam[k] - ((array[le].pos[k] + array[re].pos[k]) * 0.5f - cam[k]);

    // guarda tudo antes de mudar qualquer osso
    for (int i = 0; i < N; i++)
    {
        idx[i] = GetIndex(hier, shrink[i][0]);
        bool isLow = shrink[i][1] == BAIXO;
        int to = isLow ? 0 : GetIndex(hier, shrink[i][1]);
        if (idx[i] < 0 || idx[i] >= nodes || to < 0 || to >= nodes) { idx[i] = -1; continue; }
        saved[i] = array[idx[i]];
        memcpy(point[i], isLow ? low : array[to].pos, sizeof(point[i]));
    }
    for (int i = 0; i < N; i++)
    {
        if (idx[i] < 0) continue;
        Matrix* m = &array[idx[i]];
        // posicao invalida (NaN): o chip grafico descarta todo triangulo
        // ligado a este osso, sem deixar fio entre o braco e o tronco
        for (int k = 0; k < 3; k++)
            m->right[k] = m->up[k] = m->at[k] = m->pos[k] = __builtin_nanf("");
    }
    Render_orig(ped);
    for (int i = 0; i < N; i++)
        if (idx[i] >= 0) array[idx[i]] = saved[i];
}

extern "C" void OnModLoad()
{
    logger->SetTag("FPS Teste");
    uintptr_t lib = aml->GetLib("libGTASA.so");
    void* h = aml->GetLibHandle("libGTASA.so");
    if (!lib || !h) { Note("v21: libGTASA nao encontrada"); return; }

    FindPlayerPed     = (FindPlayerPed_t)aml->GetSym(h, "_Z13FindPlayerPedi");
    FindPlayerVehicle = (FindPlayerVehicle_t)aml->GetSym(h, "_Z17FindPlayerVehicleib");
    GetHier  = (GetHier_t)aml->GetSym(h, "_Z29GetAnimHierarchyFromSkinClumpP7RpClump");
    GetIndex = (GetIndex_t)aml->GetSym(h, "_Z17RpHAnimIDGetIndexP16RpHAnimHierarchyi");
    Camera   = (char*)aml->GetSym(h, "TheCamera");

    // posicao de CPed::Render na tabela do jogador (conferida no log da v13)
    uintptr_t* slot = (uintptr_t*)(lib + 0x6692d4);
    if (!FindPlayerPed || !FindPlayerVehicle || !GetHier || !GetIndex || !Camera
        || *slot != lib + 0x4a68f5)
    {
        Note("v21: NAO ativado (funcao ausente ou tabela diferente)");
        aml->ShowToast(true, "FPS Teste v21: NAO ativado");
        return;
    }
    Render_orig = (Render_t)*slot;
    aml->Unprot((uintptr_t)slot, sizeof(uintptr_t));
    *slot = (uintptr_t)&HookOf_Render;
    Note("v21 carregado: gancho OK");
    aml->ShowToast(true, "FPS Teste v21: ativado");
}
