// Compute: escribe un patron conocido que la CPU verifica despues
[[vk::binding(0, 0)]] RWStructuredBuffer<uint> Output : register(u0);

[numthreads(64, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    Output[id.x] = id.x * id.x + 7;
}
