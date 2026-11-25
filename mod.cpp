#include <mod/amlmod.h>
#include <mod/logger.h>
#include <mod/config.h>

MYMODCFG(net.fpsmod.teste, FPS Teste, 1.0, VoceMesmo)

extern "C" void OnModLoad()
{
    logger->SetTag("FPS Teste");
    logger->Info("Mod carregado com sucesso");
}
