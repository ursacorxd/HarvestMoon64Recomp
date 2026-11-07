// Pinta el color que decide la CPU (verde si el compute fue correcto, rojo si no)
[[vk::binding(0, 0)]] cbuffer Params : register(b0) {
    float4 Color;
};

float4 PSMain() : SV_Target {
    return Color;
}
