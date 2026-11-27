#include <mod/amlmod.h>
#include <mod/logger.h>
#include <mod/config.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <elf.h>
#include <link.h>

MYMODCFG(net.fpsmod.teste, FPS Teste, 22.0, VoceMesmo)

// v22: esconde cabeca e tronco do CJ SO quando ele esta em moto/bicicleta/quadri
// e a camera esta perto da cabeca (primeira pessoa). Nao altera arquivos.
static void Note(const char* msg)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/fpsteste.txt", aml->GetAndroidDataPath());
    FILE* f = fopen(path, "a");
    if (f) { fprintf(f, "%s\n", msg); fclose(f); }
}

// v22: SO LE a tabela de simbolos da libGTASA para achar a funcao que prende a
// camera ao CJ, e se da para interceptar. Nao altera nada.
#if __SIZEOF_POINTER__ == 4
#define REL_SYM(i) ELF32_R_SYM(i)
#else
#define REL_SYM(i) ELF64_R_SYM(i)
#endif

static bool Interessa(const char* n)
{
    if (!strcmp(n, "_ZN7CCamera7ProcessEv")) return true;
    if (strncmp(n, "_ZN7CCamera", 11) && strncmp(n, "_ZN4CCam", 8)) return false;
    return strstr(n, "Attach") || strstr(n, "TakeControl");
}

static void Diagnostico(uintptr_t lib)
{
    const ElfW(Ehdr)* eh = (const ElfW(Ehdr)*)lib;
    if (memcmp(eh->e_ident, ELFMAG, SELFMAG)) { Note("diag: cabecalho nao encontrado"); return; }
    const ElfW(Phdr)* ph = (const ElfW(Phdr)*)(lib + eh->e_phoff);
    const ElfW(Dyn)* dyn = NULL;
    for (int i = 0; i < eh->e_phnum; i++)
        if (ph[i].p_type == PT_DYNAMIC) dyn = (const ElfW(Dyn)*)(lib + ph[i].p_vaddr);
    if (!dyn) { Note("diag: sem secao dinamica"); return; }

    uintptr_t symtab = 0, strtab = 0, hash = 0, gnu = 0;
    uintptr_t tab[3] = { 0, 0, 0 };      // JMPREL, REL, RELA
    size_t tam[3] = { 0, 0, 0 };
    bool pltRela = false;
    for (; dyn->d_tag != DT_NULL; dyn++)
    {
        uintptr_t v = dyn->d_un.d_ptr;
        uintptr_t p = v < lib ? lib + v : v;
        switch (dyn->d_tag)
        {
            case DT_SYMTAB:   symtab = p; break;
            case DT_STRTAB:   strtab = p; break;
            case DT_HASH:     hash = p; break;
            case DT_GNU_HASH: gnu = p; break;
            case DT_JMPREL:   tab[0] = p; break;
            case DT_PLTRELSZ: tam[0] = v; break;
            case DT_PLTREL:   pltRela = v == DT_RELA; break;
            case DT_REL:      tab[1] = p; break;
            case DT_RELSZ:    tam[1] = v; break;
            case DT_RELA:     tab[2] = p; break;
            case DT_RELASZ:   tam[2] = v; break;
        }
    }
    if (!symtab || !strtab) { Note("diag: sem tabela de simbolos"); return; }

    uint32_t nsym = 0;
    if (hash) nsym = ((const uint32_t*)hash)[1];
    else if (gnu)
    {
        const uint32_t* g = (const uint32_t*)gnu;
        uint32_t nb = g[0], first = g[1], nbloom = g[2];
        const uint32_t* buckets = (const uint32_t*)((const ElfW(Addr)*)(g + 4) + nbloom);
        const uint32_t* chain = buckets + nb;
        uint32_t last = 0;
        for (uint32_t i = 0; i < nb; i++) if (buckets[i] > last) last = buckets[i];
        if (last >= first) { while (!(chain[last - first] & 1)) last++; nsym = last + 1; }
    }

    const ElfW(Sym)* sym = (const ElfW(Sym)*)symtab;
    char b[400];
    int achados = 0;
    for (uint32_t s = 1; s < nsym && achados < 40; s++)
    {
        const char* n = (const char*)strtab + sym[s].st_name;
        if (!Interessa(n)) continue;
        achados++;
        snprintf(b, sizeof(b), "diag: %s = 0x%lx", n, (unsigned long)sym[s].st_value);
        Note(b);
        for (int t = 0; t < 3; t++)
        {
            if (!tab[t]) continue;
            bool rela = t == 2 || (t == 0 && pltRela);
            size_t passo = rela ? sizeof(ElfW(Rela)) : sizeof(ElfW(Rel));
            for (size_t o = 0; o + passo <= tam[t]; o += passo)
            {
                const ElfW(Rel)* r = (const ElfW(Rel)*)(tab[t] + o);
                if (REL_SYM(r->r_info) != s) continue;
                uintptr_t valor = *(const uintptr_t*)(lib + r->r_offset);
                snprintf(b, sizeof(b), "diag:    entrada %d em 0x%lx, aponta para 0x%lx%s",
                         t, (unsigned long)r->r_offset, (unsigned long)(valor - lib),
                         valor == lib + sym[s].st_value ? " (a propria funcao)" : "");
                Note(b);
            }
        }
    }
    snprintf(b, sizeof(b), "diag: fim, %u simbolos, %d encontrados", nsym, achados);
    Note(b);
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
    if (!lib || !h) { Note("v22: libGTASA nao encontrada"); return; }

    Diagnostico(lib);

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
        Note("v22: NAO ativado (funcao ausente ou tabela diferente)");
        aml->ShowToast(true, "FPS Teste v22: NAO ativado");
        return;
    }
    Render_orig = (Render_t)*slot;
    aml->Unprot((uintptr_t)slot, sizeof(uintptr_t));
    *slot = (uintptr_t)&HookOf_Render;
    Note("v22 carregado: gancho OK");
    aml->ShowToast(true, "FPS Teste v22: ativado");
}
