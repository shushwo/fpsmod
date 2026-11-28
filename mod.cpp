#include <mod/amlmod.h>
#include <mod/logger.h>
#include <mod/config.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>

MYMODCFG(net.fpsmod.teste, FPS Teste, 33.0, VoceMesmo)

// v33: com a camera perto (1a pessoa), esconde cabeca e tronco em moto/bicicleta/
// quadri e so a cabeca nos outros veiculos, a pe e nadando
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

// ---- ajuste da camera por veiculo (arquivo fpsajustes.txt) ----
struct Vec3 { float x, y, z; };
struct Ajuste { int modelo; float frente, altura, inclina; };
static Ajuste gAjustes[64];
static int gNumAjustes = 0;
static float gInclinar = 1.0f;      // linha "inclinar N" (0 desliga, 1 total)
static float gBaixo = 0.5f;         // linha "baixo N": a pe, olhando mais que isso
                                    // para baixo a cabeca nao e escondida

// a pe: quanto a camera fica a frente do corpo (metros): base, extra se gordo,
// extra se musculoso. Linha "ape B G M" no fpsajustes.txt
static float gApe[3] = { 0.0f, 0.0f, 0.0f };

static const char* PADRAO =
    "# Ajuste da camera em primeira pessoa, um veiculo por linha:\n"
    "# modelo  frente  altura  inclinacao\n"
    "# frente: metros (positivo = mais para a frente, negativo = para tras)\n"
    "# altura: metros (positivo = mais alta, negativo = mais baixa)\n"
    "# inclinacao: graus (positivo = olha mais para baixo)\n"
    "# Salve o arquivo e volte ao jogo: vale em ate 2 segundos.\n"
    "# A pe: base, extra se gordo, extra se musculoso (metros)\n"
    "ape 0 0 0\n"
    "# inclinar N: helicoptero inclina a imagem junto (0 desliga, 1 total)\n"
    "inclinar 1.0\n"
    "# BMX\n"
    "481  0.00  -0.05  8\n"
    "# Bike\n"
    "509  0.00  -0.05  8\n"
    "# Mountain Bike\n"
    "510  0.00  -0.05  8\n"
    "# Freeway\n"
    "463  0.10   0.08  0\n"
    "# Sanchez\n"
    "468  0.10   0.00  0\n";

static float Limite(float v, float m) { return v != v ? 0 : v > m ? m : v < -m ? -m : v; }

static void LerAjustes(const char* caminho)
{
    FILE* f = fopen(caminho, "r");
    if (!f)
    {
        FILE* w = fopen(caminho, "w");   // cria so quando o arquivo nao existe
        if (w) { fputs(PADRAO, w); fclose(w); }
        f = fopen(caminho, "r");
        if (!f) return;
    }
    char linha[200];
    int n = 0;
    gApe[0] = gApe[1] = gApe[2] = 0; gInclinar = 1.0f; gBaixo = 0.5f;
    while (n < 64 && fgets(linha, sizeof(linha), f))
    {
        for (char* p = linha; *p; p++)
            if (*p == ',') *p = '.';      // aceita 0,10 e 0.10
        float h;
        if (sscanf(linha, " inclinar %f", &h) == 1)
        {
            gInclinar = h > 0 ? (h < 1 ? h : 1) : 0;
            continue;
        }
        if (sscanf(linha, " baixo %f", &h) == 1)
        {
            gBaixo = Limite(h, 2.0f);
            continue;
        }
        float v[3] = { gApe[0], gApe[1], gApe[2] };
        if (sscanf(linha, " ape %f %f %f", &v[0], &v[1], &v[2]) >= 1)
        {
            for (int k = 0; k < 3; k++) gApe[k] = Limite(v[k], 0.5f);
            continue;
        }
        Ajuste a = { 0, 0, 0, 0 };
        if (sscanf(linha, "%d %f %f %f", &a.modelo, &a.frente, &a.altura, &a.inclina) < 2
            || a.modelo <= 0) continue;
        a.frente = Limite(a.frente, 1.0f);
        a.altura = Limite(a.altura, 1.0f);
        a.inclina = Limite(a.inclina, 45.0f);
        gAjustes[n++] = a;
    }
    gNumAjustes = n;
    fclose(f);
}

static bool Ajustar(int modelo, Vec3* pos, Vec3* olhar)
{
    for (int i = 0; i < gNumAjustes; i++)
    {
        if (gAjustes[i].modelo != modelo) continue;
        pos->y += gAjustes[i].frente;
        pos->z += gAjustes[i].altura;
        olhar->z += gAjustes[i].altura;   // mantem a direcao do olhar
        olhar->z -= (olhar->y - pos->y) * tanf(gAjustes[i].inclina * 0.0174532925f);
        return true;
    }
    return false;
}

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

typedef float (*GetStat_t)(unsigned short);
static GetStat_t GetStat;
static time_t gUltimaLeitura = 0;
static int gLogsApe = 0;
static float gUltimoRecuo = -9.0f;

static void RecarregarAjustes()
{
    time_t agora = time(NULL);
    if (agora - gUltimaLeitura < 2 && agora >= gUltimaLeitura) return;
    char path[512];
    snprintf(path, sizeof(path), "%s/fpsajustes.txt", aml->GetAndroidDataPath());
    LerAjustes(path);
    gUltimaLeitura = agora;
}

static float Fracao(unsigned short qual)
{
    if (!GetStat) return 0;
    float v = GetStat(qual) / 1000.0f;
    return v > 0 ? (v < 1 ? v : 1) : 0;
}

// empurra o corpo desenhado para tras (o mesmo que adiantar a camera)
static void AfastarCorpo(void* ped, void* hier, Matrix* array, int nodes, const char* camMatrix)
{
    RecarregarAjustes();
    float gordo = Fracao(21), musculo = Fracao(23);
    float recuo = gApe[0] + gApe[1] * gordo + gApe[2] * musculo;
    char* m = *(char**)((char*)ped + 0x14);
    if (!m) return;
    const float* frente = (const float*)(m + 0x10);
    float x = frente[0], y = frente[1];
    float len = sqrtf(x * x + y * y);
    if (!(len > 0.3f && len < 1.1f)) return;      // so a direcao no chao
    // so afasta quando a camera olha para baixo: nada olhando para a frente,
    // aumentando aos poucos ate o valor cheio
    float baixo = -((const float*)(camMatrix + 0x10))[2];
    float quanto = (baixo - 0.30f) / 0.50f;
    quanto = quanto > 0 ? (quanto < 1 ? quanto : 1) : 0;

    // antebracos, maos e dedos ficam no lugar (a arma e desenhada a parte);
    // o braco de cima vai so pela metade, para nao esticar de repente
    float peso[100];
    for (int i = 0; i < nodes; i++) peso[i] = 1.0f;
    static const int parado[][2] = { {22,50}, {23,0}, {24,0}, {25,0}, {26,0},
                                     {32,50}, {33,0}, {34,0}, {35,0}, {36,0} };
    for (unsigned k = 0; k < sizeof(parado) / sizeof(parado[0]); k++)
    {
        int i = GetIndex(hier, parado[k][0]);
        if (i >= 0 && i < nodes) peso[i] = parado[k][1] / 100.0f;
    }
    for (int i = 0; i < nodes; i++)
    {
        array[i].pos[0] -= x / len * recuo * quanto * peso[i];
        array[i].pos[1] -= y / len * recuo * quanto * peso[i];
    }
    if (gLogsApe < 20 && fabsf(recuo - gUltimoRecuo) > 0.005f)
    {
        gUltimoRecuo = recuo; gLogsApe++;
        char b[160];
        snprintf(b, sizeof(b), "ape recuo=%.3f gordura=%.2f musculo=%.2f", recuo, gordo, musculo);
        Note(b);
    }
}

static bool IsHeliModel(int m);

static void HookOf_Render(void* ped)
{
    if (ped != FindPlayerPed(-1)) { Render_orig(ped); return; }

    void* veh = FindPlayerVehicle(-1, false);
    int model = veh ? *(uint16_t*)((char*)veh + 0x26) : 0;

    void* clump = *(void**)((char*)ped + 0x18);
    void* hier = clump ? GetHier(clump) : NULL;
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
        // so esconde com a camera perto (primeira pessoa). A pe e nadando o
        // limite e menor, para nao afetar a camera de terceira pessoa
        const float limite = veh ? 1.2f : 0.6f;
        hide = dist2 < limite * limite;
        // a pe, olhando bem para baixo: deixa a cabeca, que fecha a gola
        if (!veh && -((const float*)(camMatrix + 0x10))[2] > gBaixo) hide = false;
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
    // moto/bicicleta: lista inteira. Outros veiculos: so os 4 primeiros (cabeca)
    const int usar = IsBikeModel(model) ? N : 4;
    for (int i = 0; i < N; i++)
    {
        if (i >= usar) { idx[i] = -1; continue; }
        idx[i] = GetIndex(hier, shrink[i][0]);
        bool isLow = shrink[i][1] == BAIXO;
        int to = isLow ? 0 : GetIndex(hier, shrink[i][1]);
        if (idx[i] < 0 || idx[i] >= nodes || to < 0 || to >= nodes) { idx[i] = -1; continue; }
        saved[i] = array[idx[i]];
        memcpy(point[i], isLow ? low : array[to].pos, sizeof(point[i]));
    }
    // a pe/nadando: recolhe a cabeca para dentro do pescoco, o que mantem a gola
    // fechada (com NaN ficava um buraco e dava para ver dentro do corpo)
    int pescoco = veh ? -1 : GetIndex(hier, 4);
    float gola[3] = { 0, 0, 0 };
    if (pescoco >= 0 && pescoco < nodes) memcpy(gola, array[pescoco].pos, sizeof(gola));
    else pescoco = -1;
    for (int i = 0; i < N; i++)
    {
        if (idx[i] < 0) continue;
        Matrix* m = &array[idx[i]];
        if (pescoco >= 0)
        {
            memset(m->right, 0, sizeof(m->right));
            memset(m->up, 0, sizeof(m->up));
            memset(m->at, 0, sizeof(m->at));
            memcpy(m->pos, gola, sizeof(gola));
            continue;
        }
        // posicao invalida (NaN): o chip grafico descarta todo triangulo
        // ligado a este osso, sem deixar fio entre o braco e o tronco
        for (int k = 0; k < 3; k++)
            m->right[k] = m->up[k] = m->at[k] = m->pos[k] = __builtin_nanf("");
    }
    // a pe: guarda o esqueleto inteiro, afasta o corpo, desenha e devolve
    static Matrix inteiro[100];
    RecarregarAjustes();
    const bool afastar = !veh && (gApe[0] != 0 || gApe[1] != 0 || gApe[2] != 0);
    if (afastar)
    {
        memcpy(inteiro, array, nodes * sizeof(Matrix));
        AfastarCorpo(ped, hier, array, nodes, camMatrix);
    }
    Render_orig(ped);
    if (afastar) memcpy(array, inteiro, nodes * sizeof(Matrix));
    for (int i = 0; i < N; i++)
        if (idx[i] >= 0) array[idx[i]] = saved[i];
}

// ---- helicopteros: a camera acompanha o corpo do CJ ----
// O desenho do CJ anota onde esta o peito dele (em relacao ao proprio CJ) e a
// camera soma o quanto ele saiu da posicao sentado e parado.
static bool IsHeliModel(int m)
{
    static const int h[] = { 417,425,447,469,487,488,497,548,563 };
    for (unsigned i = 0; i < sizeof(h) / sizeof(h[0]); i++)
        if (h[i] == m) return true;
    return false;
}

// m = matriz do CJ (direita, frente, cima, posicao); osso = posicao no mundo
// ---- helicopteros: a imagem inclina e vira junto com a aeronave ----
// Depois que o jogo calcula a camera presa, troca o "lado de cima" dela pelo do
// helicoptero. So escreve se os valores lidos tiverem cara de camera.
#define CAM_FRENTE 0x190
#define CAM_CIMA   0x1B4
typedef void (*CamProc_t)(void*);
static CamProc_t CamProc_orig;
static time_t gCamPresa = 0;        // ultima vez que o script prendeu a camera

static float Pto(const float* a, const float* b) { return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]; }
static bool Unitario(const float* v) { float n = Pto(v, v); return n > 0.98f && n < 1.02f; }
static bool CaraDeCamera(const float* frente, const float* cima)
{
    return Unitario(frente) && Unitario(cima) && fabsf(Pto(frente, cima)) < 0.05f;
}

// mistura o cima da camera com o do veiculo e deixa em angulo reto com a frente
static bool Inclinar(const float* frente, float* cima, const float* veiculo, float forca)
{
    float c[3];
    for (int k = 0; k < 3; k++) c[k] = cima[k] + (veiculo[k] - cima[k]) * forca;
    float d = Pto(c, frente);
    for (int k = 0; k < 3; k++) c[k] -= frente[k] * d;
    float n = sqrtf(Pto(c, c));
    if (!(n > 0.1f)) return false;
    for (int k = 0; k < 3; k++) cima[k] = c[k] / n;
    return true;
}

static void HookOf_CamProc(void* cam)
{
    CamProc_orig(cam);
    if (gInclinar <= 0 || time(NULL) - gCamPresa > 1) return;
    void* veh = FindPlayerPed(-1) ? FindPlayerVehicle(-1, false) : NULL;
    if (!veh || !IsHeliModel(*(uint16_t*)((char*)veh + 0x26))) return;
    char* m = *(char**)((char*)veh + 0x14);
    if (!m) return;
    const float* acima = (const float*)(m + 0x20);
    float* frente = (float*)((char*)cam + CAM_FRENTE);
    float* cima = (float*)((char*)cam + CAM_CIMA);
    static bool anotado = false;
    bool ok = CaraDeCamera(frente, cima) && Unitario(acima);
    if (!anotado)
    {
        anotado = true;
        char b[220];
        snprintf(b, sizeof(b), "inclinar: %s frente=%.2f %.2f %.2f cima=%.2f %.2f %.2f",
                 ok ? "OK" : "valores nao conferem",
                 frente[0], frente[1], frente[2], cima[0], cima[1], cima[2]);
        Note(b);
        // se nao conferir, anota onde ha vetores de tamanho 1 para corrigir depois
        for (int o = 0x100; !ok && o <= 0x220; o += 4)
        {
            const float* v = (const float*)((char*)cam + o);
            if (!Unitario(v)) continue;
            snprintf(b, sizeof(b), "inclinar:  em 0x%x: %.2f %.2f %.2f", o, v[0], v[1], v[2]);
            Note(b);
        }
    }
    if (ok) Inclinar(frente, cima, acima, gInclinar);
}

typedef void (*Attach_t)(void*, void*, void*, Vec3*, Vec3*, float, short, int);
static Attach_t Attach_orig;
static int gUltimoCam = -1;
static int gLogsCam = 0;

// o script chama isto para prender a camera ao CJ; aqui somamos o ajuste
static void HookOf_Attach(void* cam, void* alvo, void* preso, Vec3* pos, Vec3* olhar,
                          float tilt, short troca, int quem)
{
    void* cj = FindPlayerPed(-1);
    void* veh = cj ? FindPlayerVehicle(-1, false) : NULL;
    if (!veh || !pos || !olhar || (preso != cj && alvo != cj))
    {
        Attach_orig(cam, alvo, preso, pos, olhar, tilt, troca, quem);
        return;
    }
    int modelo = *(uint16_t*)((char*)veh + 0x26);

    RecarregarAjustes();

    Vec3 p = *pos, o = *olhar;       // copias: nao mexe nos valores do jogo
    bool mudou = Ajustar(modelo, &p, &o);
    gCamPresa = time(NULL);

    if (modelo != gUltimoCam && gLogsCam < 100)
    {
        gUltimoCam = modelo; gLogsCam++;
        char b[220];
        snprintf(b, sizeof(b),
                 "camera modelo=%d ajuste=%s pos=%.2f %.2f %.2f olhar=%.2f %.2f %.2f",
                 modelo, mudou ? "sim" : "nao", p.x, p.y, p.z, o.x, o.y, o.z);
        Note(b);
    }
    Attach_orig(cam, alvo, preso, &p, &o, tilt, troca, quem);
}

extern "C" void OnModLoad()
{
    logger->SetTag("FPS Teste");
    uintptr_t lib = aml->GetLib("libGTASA.so");
    void* h = aml->GetLibHandle("libGTASA.so");
    if (!lib || !h) { Note("v33: libGTASA nao encontrada"); return; }

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
        Note("v33: NAO ativado (funcao ausente ou tabela diferente)");
        aml->ShowToast(true, "FPS Teste v33: NAO ativado");
        return;
    }
    Render_orig = (Render_t)*slot;
    aml->Unprot((uintptr_t)slot, sizeof(uintptr_t));
    *slot = (uintptr_t)&HookOf_Render;
    Note("v33 carregado: gancho OK");

    GetStat = (GetStat_t)aml->GetSym(h, "_ZN6CStats12GetStatValueEt");
    if (!GetStat) GetStat = (GetStat_t)aml->GetSym(h, "_ZN6CStats12GetStatValueE6eStats");
    Note(GetStat ? "v33: gordura e musculo OK" : "v33: gordura e musculo indisponiveis");

    uintptr_t* slotProc = (uintptr_t*)(lib + 0x675614);
    if (*slotProc == lib + 0x3cd4d5)
    {
        CamProc_orig = (CamProc_t)*slotProc;
        aml->Unprot((uintptr_t)slotProc, sizeof(uintptr_t));
        *slotProc = (uintptr_t)&HookOf_CamProc;
        Note("v33: inclinacao do helicoptero OK");
    }
    else Note("v33: inclinacao NAO ativada (entrada diferente)");

    // entrada por onde o jogo prende a camera (conferida no log da v23)
    uintptr_t* slotCam = (uintptr_t*)(lib + 0x66ead8);
    if (*slotCam == lib + 0x3e17e9)
    {
        Attach_orig = (Attach_t)*slotCam;
        aml->Unprot((uintptr_t)slotCam, sizeof(uintptr_t));
        *slotCam = (uintptr_t)&HookOf_Attach;
        char path[512];
        snprintf(path, sizeof(path), "%s/fpsajustes.txt", aml->GetAndroidDataPath());
        LerAjustes(path);
        Note("v33: ajuste de camera OK");
    }
    else Note("v33: ajuste de camera NAO ativado (entrada diferente)");
    aml->ShowToast(true, "FPS Teste v33: ativado");
}
