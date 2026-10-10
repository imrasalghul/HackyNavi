// SPDX-License-Identifier: MIT
#pragma once
#include "graphics_shader.hpp"
#include "graphics_snapshot.hpp"
#include "graphics_buffer_snapshot.hpp"

namespace hackynavi { namespace compute {
// Offline only. Compiles the actual validated stage SPIR-V with bundled Mesa/
// ACO, then independently decodes the owned machine bytes. Opens no GPU.
Result buildGraphicsPipeline(const GraphicsShader& vertex, const GraphicsShader& fragment,
    std::shared_ptr<const GraphicsPipeline>& output, std::string& diagnostic);
// The explicit new stage factory and a distinct immutable result prevent this
// memory-reading program from entering the old resource-free submission API.
Result buildFragmentBufferGraphicsPipeline(const GraphicsShader& vertex, const GraphicsShader& fragment,
    std::shared_ptr<const FragmentBufferGraphicsPipeline>& output, std::string& diagnostic);
Result buildVertexBufferGraphicsPipeline(const GraphicsShader& vertex, const GraphicsShader& fragment,
    std::shared_ptr<const VertexBufferGraphicsPipeline>& output, std::string& diagnostic);
Result buildVertexFragmentBufferGraphicsPipeline(const GraphicsShader& vertex, const GraphicsShader& fragment,
    std::shared_ptr<const VertexFragmentBufferGraphicsPipeline>& output, std::string& diagnostic);
} }
