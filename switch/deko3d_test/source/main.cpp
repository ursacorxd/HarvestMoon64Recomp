// Prueba minima de deko3d antes de escribir un backend de plume para RT64.
// Valida: dispositivo, swapchain, clear, un compute shader con verificacion en CPU
// y un draw con vertex/pixel shader, todos compilados HLSL -> SPIR-V -> GLSL -> DKSH.
//
// Resultado en pantalla:
//   borde azul + centro verde   -> todo correcto
//   borde azul + centro rojo    -> el compute no escribio los valores esperados
//   todo azul                   -> el draw no produjo pixeles
//   todo magenta                -> no se pudieron cargar los .dksh
//   negro / cuelgue             -> fallo el dispositivo o el swapchain
// El detalle queda en sdmc:/deko3d_test.log. Pulsa + para salir.

#include <switch.h>
#include <deko3d.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>

namespace {

constexpr uint32_t FbWidth = 1280;
constexpr uint32_t FbHeight = 720;
constexpr uint32_t NumFbs = 2;
constexpr uint32_t NumValues = 256;
constexpr uint32_t ThreadsPerGroup = 64; // debe coincidir con numthreads de test_cs.hlsl

constexpr uint32_t CmdMemSize = 64 * 1024;
constexpr uint32_t CodeMemSize = 64 * 1024;
constexpr uint32_t StorageOffset = 0;
constexpr uint32_t StorageSize = NumValues * sizeof(uint32_t);
constexpr uint32_t UniformOffset = 0x400; // multiplo de DK_UNIFORM_BUF_ALIGNMENT
constexpr uint32_t UniformSize = DK_UNIFORM_BUF_ALIGNMENT;
constexpr uint32_t DataMemSize = DK_MEMBLOCK_ALIGNMENT;
static_assert(StorageOffset + StorageSize <= UniformOffset, "el storage buffer pisa al uniform buffer");
static_assert(UniformOffset + UniformSize <= DataMemSize, "los buffers no caben en el bloque de datos");

// Copia de DkshHeader (source/dksh.h de deko3d, no se instala con la libreria)
struct DkshHeader {
    uint32_t magic;
    uint32_t header_sz;
    uint32_t control_sz;
    uint32_t code_sz;
    uint32_t programs_off;
    uint32_t num_programs;
};
constexpr uint32_t DkshMagic = 0x48534B44; // "DKSH"

FILE* g_log = nullptr;

void logMsg(const char* fmt, ...) {
    va_list va;
    va_start(va, fmt);
    vfprintf(g_log != nullptr ? g_log : stderr, fmt, va);
    va_end(va);
    if (g_log != nullptr) {
        fflush(g_log);
    }
}

void debugCallback(void*, const char* context, DkResult result, const char* message) {
    logMsg("deko3d [%s] resultado=%d: %s\n", context, (int)result, message != nullptr ? message : "");
}

uint32_t alignUp(uint32_t value, uint32_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

// Carga el .dksh completo (control + codigo) en la memoria de codigo; deko3d lee
// el control desde ahi cuando DkShaderMaker::control es NULL.
bool loadShader(DkShader* shader, const char* path, DkMemBlock codeMem, uint32_t* codeOffset) {
    FILE* f = fopen(path, "rb");
    if (f == nullptr) {
        logMsg("No se pudo abrir %s\n", path);
        return false;
    }

    DkshHeader hdr;
    if (fread(&hdr, sizeof(hdr), 1, f) != 1 || hdr.magic != DkshMagic) {
        logMsg("%s no es un DKSH valido\n", path);
        fclose(f);
        return false;
    }

    uint32_t fileSize = hdr.control_sz + hdr.code_sz;
    uint32_t usable = dkMemBlockGetSize(codeMem) - DK_SHADER_CODE_UNUSABLE_SIZE;
    if (*codeOffset + fileSize > usable) {
        logMsg("%s no cabe en la memoria de codigo (%u bytes)\n", path, fileSize);
        fclose(f);
        return false;
    }

    uint8_t* dst = (uint8_t*)dkMemBlockGetCpuAddr(codeMem) + *codeOffset;
    rewind(f);
    bool ok = fread(dst, 1, fileSize, f) == fileSize;
    fclose(f);
    if (!ok) {
        logMsg("Lectura incompleta de %s\n", path);
        return false;
    }

    DkShaderMaker maker;
    dkShaderMakerDefaults(&maker, codeMem, *codeOffset);
    dkShaderInitialize(shader, &maker);
    logMsg("Shader %s cargado (control=%u codigo=%u offset=0x%x)\n", path, hdr.control_sz, hdr.code_sz, *codeOffset);

    *codeOffset = alignUp(*codeOffset + fileSize, DK_SHADER_CODE_ALIGNMENT);
    return true;
}

// Ejecuta test_cs, espera a la GPU y comprueba los valores en la CPU
bool runComputeTest(DkQueue queue, DkCmdBuf cmdbuf, const DkShader* computeShader, DkMemBlock dataMem) {
    uint32_t* values = (uint32_t*)((uint8_t*)dkMemBlockGetCpuAddr(dataMem) + StorageOffset);
    for (uint32_t i = 0; i < NumValues; i++) {
        values[i] = 0xDEADBEEF;
    }

    DkShader const* shaders[] = { computeShader };
    dkCmdBufBindShaders(cmdbuf, DkStageFlag_Compute, shaders, 1);
    dkCmdBufBindStorageBuffer(cmdbuf, DkStage_Compute, 0, dkMemBlockGetGpuAddr(dataMem) + StorageOffset, StorageSize);
    dkCmdBufDispatchCompute(cmdbuf, NumValues / ThreadsPerGroup, 1, 1);
    dkQueueSubmitCommands(queue, dkCmdBufFinishList(cmdbuf));
    // waitIdle senaliza un fence con flush, asi que la L2 de la GPU llega a memoria
    dkQueueWaitIdle(queue);

    uint32_t errors = 0;
    for (uint32_t i = 0; i < NumValues; i++) {
        uint32_t expected = i * i + 7;
        if (values[i] != expected) {
            if (errors < 8) {
                logMsg("Compute: valor[%u] = 0x%08x, esperado 0x%08x\n", i, values[i], expected);
            }
            errors++;
        }
    }
    logMsg("Compute: %u/%u valores correctos\n", NumValues - errors, NumValues);
    return errors == 0;
}

} // namespace

int main(int, char**) {
    g_log = fopen("sdmc:/deko3d_test.log", "w");
    logMsg("deko3d test iniciado\n");

    Result rc = romfsInit();
    if (R_FAILED(rc)) {
        logMsg("romfsInit fallo: 0x%x\n", rc);
    }

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    DkDeviceMaker deviceMaker;
    dkDeviceMakerDefaults(&deviceMaker);
    deviceMaker.cbDebug = debugCallback;
    DkDevice device = dkDeviceCreate(&deviceMaker);
    logMsg("Dispositivo creado\n");

    // Framebuffers y swapchain
    DkImageLayoutMaker layoutMaker;
    dkImageLayoutMakerDefaults(&layoutMaker, device);
    layoutMaker.flags = DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_HwCompression;
    layoutMaker.format = DkImageFormat_RGBA8_Unorm;
    layoutMaker.dimensions[0] = FbWidth;
    layoutMaker.dimensions[1] = FbHeight;
    DkImageLayout fbLayout;
    dkImageLayoutInitialize(&fbLayout, &layoutMaker);

    uint32_t fbSize = alignUp((uint32_t)dkImageLayoutGetSize(&fbLayout), dkImageLayoutGetAlignment(&fbLayout));
    DkMemBlockMaker memMaker;
    dkMemBlockMakerDefaults(&memMaker, device, alignUp(NumFbs * fbSize, DK_MEMBLOCK_ALIGNMENT));
    memMaker.flags = DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image;
    DkMemBlock fbMem = dkMemBlockCreate(&memMaker);

    DkImage framebuffers[NumFbs];
    DkImage const* fbPtrs[NumFbs];
    for (uint32_t i = 0; i < NumFbs; i++) {
        dkImageInitialize(&framebuffers[i], &fbLayout, fbMem, i * fbSize);
        fbPtrs[i] = &framebuffers[i];
    }

    DkSwapchainMaker swapchainMaker;
    dkSwapchainMakerDefaults(&swapchainMaker, device, nwindowGetDefault(), fbPtrs, NumFbs);
    DkSwapchain swapchain = dkSwapchainCreate(&swapchainMaker);
    logMsg("Swapchain creado (%ux%u, %u bytes por imagen)\n", FbWidth, FbHeight, fbSize);

    // Memoria de comandos, codigo y datos
    dkMemBlockMakerDefaults(&memMaker, device, CmdMemSize);
    DkMemBlock cmdMem = dkMemBlockCreate(&memMaker);

    dkMemBlockMakerDefaults(&memMaker, device, CodeMemSize);
    memMaker.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code;
    DkMemBlock codeMem = dkMemBlockCreate(&memMaker);

    dkMemBlockMakerDefaults(&memMaker, device, DataMemSize);
    DkMemBlock dataMem = dkMemBlockCreate(&memMaker);

    DkCmdBufMaker cmdbufMaker;
    dkCmdBufMakerDefaults(&cmdbufMaker, device);
    DkCmdBuf cmdbuf = dkCmdBufCreate(&cmdbufMaker);
    dkCmdBufAddMemory(cmdbuf, cmdMem, 0, CmdMemSize);

    DkQueueMaker queueMaker;
    dkQueueMakerDefaults(&queueMaker, device);
    queueMaker.flags = DkQueueFlags_Graphics | DkQueueFlags_Compute;
    DkQueue queue = dkQueueCreate(&queueMaker);

    DkShader computeShader, vertexShader, pixelShader;
    uint32_t codeOffset = 0;
    bool shadersLoaded =
        loadShader(&computeShader, "romfs:/shaders/test_cs.dksh", codeMem, &codeOffset) &&
        loadShader(&vertexShader, "romfs:/shaders/fullscreen_vs.dksh", codeMem, &codeOffset) &&
        loadShader(&pixelShader, "romfs:/shaders/color_ps.dksh", codeMem, &codeOffset);

    bool computeOk = shadersLoaded && runComputeTest(queue, cmdbuf, &computeShader, dataMem);
    if (dkQueueIsInErrorState(queue)) {
        logMsg("La cola quedo en estado de error tras el compute\n");
    }

    float* color = (float*)((uint8_t*)dkMemBlockGetCpuAddr(dataMem) + UniformOffset);
    color[0] = computeOk ? 0.0f : 1.0f;
    color[1] = computeOk ? 1.0f : 0.0f;
    color[2] = 0.0f;
    color[3] = 1.0f;

    // Una lista por framebuffer para enlazarlo como render target
    DkCmdList bindFbCmds[NumFbs];
    for (uint32_t i = 0; i < NumFbs; i++) {
        DkImageView colorTarget;
        dkImageViewDefaults(&colorTarget, &framebuffers[i]);
        dkCmdBufBindRenderTarget(cmdbuf, &colorTarget, nullptr);
        bindFbCmds[i] = dkCmdBufFinishList(cmdbuf);
    }

    // Lista de render: clear a pantalla completa y triangulo en el centro
    DkViewport fullViewport = { 0.0f, 0.0f, (float)FbWidth, (float)FbHeight, 0.0f, 1.0f };
    DkViewport centerViewport = { FbWidth / 4.0f, FbHeight / 4.0f, FbWidth / 2.0f, FbHeight / 2.0f, 0.0f, 1.0f };
    DkScissor fullScissor = { 0, 0, FbWidth, FbHeight };
    dkCmdBufSetViewports(cmdbuf, 0, &fullViewport, 1);
    dkCmdBufSetScissors(cmdbuf, 0, &fullScissor, 1);
    if (shadersLoaded) {
        dkCmdBufClearColorFloat(cmdbuf, 0, DkColorMask_RGBA, 0.0f, 0.0f, 1.0f, 1.0f);

        DkRasterizerState rasterizerState;
        dkRasterizerStateDefaults(&rasterizerState);
        rasterizerState.cullMode = DkFace_None;
        DkColorState colorState;
        dkColorStateDefaults(&colorState);
        DkColorWriteState colorWriteState;
        dkColorWriteStateDefaults(&colorWriteState);

        DkShader const* graphicsShaders[] = { &vertexShader, &pixelShader };
        dkCmdBufBindShaders(cmdbuf, DkStageFlag_GraphicsMask, graphicsShaders, 2);
        dkCmdBufBindUniformBuffer(cmdbuf, DkStage_Fragment, 0, dkMemBlockGetGpuAddr(dataMem) + UniformOffset, UniformSize);
        dkCmdBufBindRasterizerState(cmdbuf, &rasterizerState);
        dkCmdBufBindColorState(cmdbuf, &colorState);
        dkCmdBufBindColorWriteState(cmdbuf, &colorWriteState);
        dkCmdBufBindVtxAttribState(cmdbuf, nullptr, 0);
        dkCmdBufBindVtxBufferState(cmdbuf, nullptr, 0);
        dkCmdBufSetViewports(cmdbuf, 0, &centerViewport, 1);
        dkCmdBufDraw(cmdbuf, DkPrimitive_Triangles, 3, 1, 0, 0);
    }
    else {
        dkCmdBufClearColorFloat(cmdbuf, 0, DkColorMask_RGBA, 1.0f, 0.0f, 1.0f, 1.0f);
    }
    DkCmdList renderCmds = dkCmdBufFinishList(cmdbuf);

    logMsg("Resultado: shaders=%s compute=%s. Pulsa + para salir\n", shadersLoaded ? "ok" : "fallo", computeOk ? "ok" : "fallo");

    bool loggedFirstFrame = false;
    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus) {
            break;
        }

        int slot = dkQueueAcquireImage(queue, swapchain);
        dkQueueSubmitCommands(queue, bindFbCmds[slot]);
        dkQueueSubmitCommands(queue, renderCmds);
        dkQueuePresentImage(queue, swapchain, slot);

        if (!loggedFirstFrame) {
            dkQueueWaitIdle(queue);
            logMsg("Primer frame presentado%s\n", dkQueueIsInErrorState(queue) ? " (cola en estado de error)" : "");
            loggedFirstFrame = true;
        }
    }

    dkQueueWaitIdle(queue);
    dkQueueDestroy(queue);
    dkCmdBufDestroy(cmdbuf);
    dkSwapchainDestroy(swapchain);
    dkMemBlockDestroy(dataMem);
    dkMemBlockDestroy(codeMem);
    dkMemBlockDestroy(cmdMem);
    dkMemBlockDestroy(fbMem);
    dkDeviceDestroy(device);

    logMsg("Fin\n");
    romfsExit();
    if (g_log != nullptr) {
        fclose(g_log);
    }
    return 0;
}
