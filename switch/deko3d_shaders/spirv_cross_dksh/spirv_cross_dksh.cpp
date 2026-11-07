// Convierte el SPIR-V que dxc genera para Vulkan en GLSL que acepta uam (deko3d), y escribe
// la tabla que usara el backend deko3d de plume para traducir cada (set, binding) de Vulkan
// al slot correspondiente de deko3d. Equivalente a src/tools/spirv_cross_msl de RT64.
//
// Slots de deko3d (por etapa): 16 UBOs, 16 SSBOs, 32 texturas, 8 imagenes.
//   UBO 0: push constants
//   UBO 1: tabla bindless (solo si el shader usa arrays de texturas)
//   UBO 2..15: constant buffers
// Los arrays de texturas (p. ej. gTextures[8192] de RasterPS) no caben en 32 slots, asi que
// se acceden con handles bindless: handle = (base de la imagen + indice) | bits del sampler,
// ambos leidos de la tabla bindless que rellena el backend.
//
// Formato del .map (una linea por recurso):
//   ubo <slot> push
//   ubo <slot> <set> <binding>
//   ssbo <slot> <set> <binding>
//   image <slot> <set> <binding>
//   texture <slot> <set> <binding> <set del sampler|-> <binding del sampler|->
//   bindless <indice> <set> <binding> <set del sampler|-> <binding del sampler|->
//   bindless_info <slot>

#include "spirv_glsl.hpp"
#include "spirv_parser.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

using namespace spirv_cross;

namespace {

constexpr uint32_t MaxUniformBuffers = 16;
constexpr uint32_t MaxStorageBuffers = 16;
constexpr uint32_t MaxTextures = 32;
constexpr uint32_t MaxImages = 8;
constexpr uint32_t PushConstantSlot = 0;
constexpr uint32_t BindlessInfoSlot = 1;
constexpr uint32_t FirstUniformSlot = 2;
const std::string BindlessPrefix = "DkBindless";
const std::string BindlessInfoName = "dkBindless";

std::vector<uint32_t> readSpirv(const char* path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        throw std::runtime_error(std::string("No se pudo abrir ") + path);
    }
    std::streamsize size = file.tellg();
    file.seekg(0);
    std::vector<uint32_t> words(size / sizeof(uint32_t));
    file.read(reinterpret_cast<char*>(words.data()), words.size() * sizeof(uint32_t));
    return words;
}

void writeFile(const char* path, const std::string& contents) {
    std::ofstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error(std::string("No se pudo escribir ") + path);
    }
    file << contents;
}

struct Binding {
    uint32_t set;
    uint32_t binding;
};

Binding getBinding(const CompilerGLSL& glsl, ID id) {
    return { glsl.get_decoration(id, spv::DecorationDescriptorSet), glsl.get_decoration(id, spv::DecorationBinding) };
}

std::string formatBinding(const CompilerGLSL& glsl, ID id) {
    Binding b = getBinding(glsl, id);
    return std::to_string(b.set) + " " + std::to_string(b.binding);
}

// Ordena por (set, binding) para que los slots no dependan del orden de declaracion
SmallVector<Resource> sortedByBinding(const CompilerGLSL& glsl, SmallVector<Resource> resources) {
    std::sort(resources.begin(), resources.end(), [&](const Resource& a, const Resource& b) {
        Binding ba = getBinding(glsl, a.id);
        Binding bb = getBinding(glsl, b.id);
        return ba.set != bb.set ? ba.set < bb.set : ba.binding < bb.binding;
    });
    return resources;
}

void checkLimit(uint32_t used, uint32_t limit, const char* kind) {
    if (used > limit) {
        throw std::runtime_error(std::string("Demasiados recursos de tipo ") + kind + ": " + std::to_string(used) + " (maximo " + std::to_string(limit) + ")");
    }
}

// Sustituye cada DkBindless<k>[expr] por un sampler construido desde un handle bindless
std::string rewriteBindless(std::string source, uint32_t count) {
    // dxc marca los indices con NonUniformResourceIndex; los handles bindless no lo necesitan
    source = std::regex_replace(source, std::regex("#extension GL_EXT_nonuniform_qualifier : require\n"), "");
    source = std::regex_replace(source, std::regex("nonuniformEXT\\("), "(");

    for (uint32_t k = 0; k < count; k++) {
        std::string name = BindlessPrefix + std::to_string(k);
        std::regex declRegex("(layout\\([^)]*\\) )?uniform (\\w+) " + name + "\\[\\d*\\];\n");
        std::smatch declMatch;
        if (!std::regex_search(source, declMatch, declRegex)) {
            throw std::runtime_error("No se encontro la declaracion de " + name);
        }
        std::string samplerType = declMatch[2].str();
        source = declMatch.prefix().str() + declMatch.suffix().str();

        std::string access = name + "[";
        size_t pos;
        while ((pos = source.find(access)) != std::string::npos) {
            size_t start = pos + access.size();
            size_t end = start;
            for (int depth = 1; depth > 0; end++) {
                if (end >= source.size()) {
                    throw std::runtime_error("Corchetes sin cerrar en el acceso a " + name);
                }
                if (source[end] == '[') {
                    depth++;
                }
                else if (source[end] == ']') {
                    depth--;
                }
            }
            std::string index = source.substr(start, end - 1 - start);
            std::string info = BindlessInfoName + "[" + std::to_string(k) + "]";
            std::string handle = samplerType + "(uvec2((" + info + ".x + uint(" + index + ")) | " + info + ".y, 0u))";
            source.replace(pos, end - pos, handle);
        }
    }
    return source;
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc != 4) {
        std::cerr << "Uso: " << argv[0] << " <entrada.spv> <salida.glsl> <salida.map>\n";
        return 1;
    }

    try {
        Parser parser(readSpirv(argv[1]));
        parser.parse();
        CompilerGLSL glsl(std::move(parser.get_parsed_ir()));

        CompilerGLSL::Options options;
        options.version = 460;
        options.es = false;
        options.vulkan_semantics = false;
        options.emit_push_constant_as_uniform_buffer = true;
        // Sin flip_vert_y: el dispositivo deko3d usa las convenciones de Vulkan (YAxisPointsDown)
        glsl.set_common_options(options);

        // HLSL usa texturas y samplers separados; GLSL de escritorio necesita combinarlos
        glsl.build_dummy_sampler_for_combined_images();
        glsl.build_combined_image_samplers();

        ShaderResources resources = glsl.get_shader_resources();
        std::ostringstream map;

        for (const Resource& r : resources.push_constant_buffers) {
            glsl.set_decoration(r.id, spv::DecorationBinding, PushConstantSlot);
            map << "ubo " << PushConstantSlot << " push\n";
        }

        uint32_t uniformSlot = FirstUniformSlot;
        for (const Resource& r : sortedByBinding(glsl, resources.uniform_buffers)) {
            map << "ubo " << uniformSlot << " " << formatBinding(glsl, r.id) << "\n";
            glsl.set_decoration(r.id, spv::DecorationBinding, uniformSlot++);
        }
        checkLimit(uniformSlot, MaxUniformBuffers, "UBO");

        uint32_t storageSlot = 0;
        for (const Resource& r : sortedByBinding(glsl, resources.storage_buffers)) {
            map << "ssbo " << storageSlot << " " << formatBinding(glsl, r.id) << "\n";
            glsl.set_decoration(r.id, spv::DecorationBinding, storageSlot++);
        }
        checkLimit(storageSlot, MaxStorageBuffers, "SSBO");

        uint32_t imageSlot = 0;
        for (const Resource& r : sortedByBinding(glsl, resources.storage_images)) {
            map << "image " << imageSlot << " " << formatBinding(glsl, r.id) << "\n";
            glsl.set_decoration(r.id, spv::DecorationBinding, imageSlot++);
        }
        checkLimit(imageSlot, MaxImages, "imagen");

        auto combined = glsl.get_combined_image_samplers();
        std::sort(combined.begin(), combined.end(), [&](const CombinedImageSampler& a, const CombinedImageSampler& b) {
            Binding ia = getBinding(glsl, a.image_id), ib = getBinding(glsl, b.image_id);
            if (ia.set != ib.set || ia.binding != ib.binding) {
                return ia.set != ib.set ? ia.set < ib.set : ia.binding < ib.binding;
            }
            Binding sa = getBinding(glsl, a.sampler_id), sb = getBinding(glsl, b.sampler_id);
            return sa.set != sb.set ? sa.set < sb.set : sa.binding < sb.binding;
        });

        uint32_t textureSlot = 0;
        uint32_t bindlessCount = 0;
        for (const CombinedImageSampler& c : combined) {
            // El sampler "dummy" lo crea spirv-cross para texelFetch: no existe en el layout de plume
            bool hasSampler = glsl.has_decoration(c.sampler_id, spv::DecorationBinding);
            std::string sampler = hasSampler ? formatBinding(glsl, c.sampler_id) : "- -";
            const SPIRType& imageType = glsl.get_type_from_variable(c.image_id);
            if (imageType.array.empty()) {
                map << "texture " << textureSlot << " " << formatBinding(glsl, c.image_id) << " " << sampler << "\n";
                glsl.set_decoration(c.combined_id, spv::DecorationBinding, textureSlot++);
            }
            else {
                map << "bindless " << bindlessCount << " " << formatBinding(glsl, c.image_id) << " " << sampler << "\n";
                glsl.set_name(c.combined_id, BindlessPrefix + std::to_string(bindlessCount++));
            }
        }
        checkLimit(textureSlot, MaxTextures, "textura");

        if (bindlessCount > 0) {
            glsl.require_extension("GL_ARB_bindless_texture");
            // x: indice de la primera imagen del array en el descriptor set de deko3d; y: sampler << 20
            glsl.add_header_line("layout(binding = " + std::to_string(BindlessInfoSlot) + ", std140) uniform DkBindlessInfo { uvec4 " +
                BindlessInfoName + "[" + std::to_string(bindlessCount) + "]; };");
            map << "bindless_info " << BindlessInfoSlot << "\n";
        }

        std::string source = rewriteBindless(glsl.compile(), bindlessCount);
        writeFile(argv[2], source);
        writeFile(argv[3], map.str());
        return 0;
    }
    catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}
