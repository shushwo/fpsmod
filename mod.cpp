#include <mod/amlmod.h>
#include <mod/logger.h>
#include <mod/config.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <elf.h>
#include <link.h>

MYMODCFG(net.fpsmod.teste, FPS Teste, 23.0, VoceMesmo)

// v23: esconde cabeca e tronco do CJ SO quando ele esta em moto/bicicleta/quadri
// e a camera esta perto da cabeca (primeira pessoa). Nao altera arquivos.
static void Note(const char* msg)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/fpsteste.txt", aml->GetAndroidDataPath());
    FILE* f = fopen(path, "a");
    if (f) { fprintf(f, "%s\n", msg); fclose(f); }
}

// v23: SO LE o arquivo libGTASA.so para achar a funcao que prende a camera ao
// CJ, e se da para interceptar. Nao altera nada.
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

// procura o caminho do arquivo da biblioteca no mapa de memoria do jogo
static bool AcharCaminho(char* caminho, size_t tam)
{
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return false;
    char linha[600];
    bool ok = false;
    while (!ok && fgets(linha, sizeof(linha), f))
    {
        char* p = strchr(linha, '/');
        if (!p || !strstr(p, "libGTASA")) continue;
        p[strcspn(p, "\n")] = 0;
        snprintf(caminho, tam, "%s", p);
        ok = true;
    }
    fclose(f);
    return ok;
}

// so deixa ler a memoria se o endereco estiver numa area legivel
static bool Legivel(uintptr_t a)
{
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return false;
    char linha[600];
    bool ok = false;
    while (!ok && fgets(linha, sizeof(linha), f))
    {
        unsigned long ini, fim; char perm[8];
        if (sscanf(linha, "%lx-%lx %7s", &ini, &fim, perm) == 3)
            ok = a >= ini && a + sizeof(uintptr_t) <= fim && perm[0] == 'r';
    }
    fclose(f);
    return ok;
}

static void* Ler(FILE* f, size_t pos, size_t tam)
{
    if (!tam || tam > 32u * 1024 * 1024) return NULL;
    char* m = (char*)malloc(tam + 1);
    if (!m) return NULL;
    if (fseek(f, (long)pos, SEEK_SET) || fread(m, 1, tam, f) != tam) { free(m); return NULL; }
    m[tam] = 0;
    return m;
}

static void Diagnostico(uintptr_t lib)
{
    char caminho[512], b[700];
    if (!AcharCaminho(caminho, sizeof(caminho))) { Note("diag: caminho nao encontrado"); return; }
    snprintf(b, sizeof(b), "diag: arquivo %s", caminho);
    Note(b);
    FILE* f = fopen(caminho, "rb");
    if (!f) { Note("diag: nao consegui abrir o arquivo"); return; }

    ElfW(Ehdr) eh;
    if (fread(&eh, 1, sizeof(eh), f) != sizeof(eh) || memcmp(eh.e_ident, ELFMAG, SELFMAG)
        || eh.e_shentsize != sizeof(ElfW(Shdr)))
    { Note("diag: cabecalho inesperado"); fclose(f); return; }
    ElfW(Shdr)* sh = (ElfW(Shdr)*)Ler(f, eh.e_shoff, (size_t)eh.e_shnum * sizeof(ElfW(Shdr)));
    if (!sh) { Note("diag: sem tabela de secoes"); fclose(f); return; }

    int ds = -1;
    for (int i = 0; i < eh.e_shnum; i++)
        if (sh[i].sh_type == SHT_DYNSYM) ds = i;
    if (ds < 0 || sh[ds].sh_link >= eh.e_shnum)
    { Note("diag: sem tabela de simbolos"); free(sh); fclose(f); return; }

    ElfW(Sym)* sym = (ElfW(Sym)*)Ler(f, sh[ds].sh_offset, sh[ds].sh_size);
    char* str = (char*)Ler(f, sh[sh[ds].sh_link].sh_offset, sh[sh[ds].sh_link].sh_size);
    size_t nsym = sym ? sh[ds].sh_size / sizeof(ElfW(Sym)) : 0;
    size_t nstr = str ? sh[sh[ds].sh_link].sh_size : 0;

    int achados = 0;
    for (size_t s = 1; s < nsym && achados < 40; s++)
    {
        if (sym[s].st_name >= nstr || !Interessa(str + sym[s].st_name)) continue;
        achados++;
        snprintf(b, sizeof(b), "diag: %s = 0x%lx", str + sym[s].st_name,
                 (unsigned long)sym[s].st_value);
        Note(b);
        // entradas por onde o jogo chama essa funcao
        for (int i = 0; i < eh.e_shnum; i++)
        {
            if ((sh[i].sh_type != SHT_REL && sh[i].sh_type != SHT_RELA) || !sh[i].sh_entsize) continue;
            char* rel = (char*)Ler(f, sh[i].sh_offset, sh[i].sh_size);
            if (!rel) continue;
            for (size_t o = 0; o + sh[i].sh_entsize <= sh[i].sh_size; o += sh[i].sh_entsize)
            {
                const ElfW(Rel)* r = (const ElfW(Rel)*)(rel + o);
                if (REL_SYM(r->r_info) != s) continue;
                uintptr_t onde = lib + r->r_offset;
                if (!Legivel(onde)) continue;
                uintptr_t valor = *(const uintptr_t*)onde;
                snprintf(b, sizeof(b), "diag:    entrada em 0x%lx, aponta para 0x%lx%s",
                         (unsigned long)r->r_offset, (unsigned long)(valor - lib),
                         valor == lib + sym[s].st_value ? " (a propria funcao)" : "");
                Note(b);
            }
            free(rel);
        }
    }
    snprintf(b, sizeof(b), "diag: fim, %lu simbolos, %d encontrados", (unsigned long)nsym, achados);
    Note(b);
    free(sym); free(str); free(sh); fclose(f);
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
    if (!lib || !h) { Note("v23: libGTASA nao encontrada"); return; }

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
        Note("v23: NAO ativado (funcao ausente ou tabela diferente)");
        aml->ShowToast(true, "FPS Teste v23: NAO ativado");
        return;
    }
    Render_orig = (Render_t)*slot;
    aml->Unprot((uintptr_t)slot, sizeof(uintptr_t));
    *slot = (uintptr_t)&HookOf_Render;
    Note("v23 carregado: gancho OK");
    aml->ShowToast(true, "FPS Teste v23: ativado");
}
