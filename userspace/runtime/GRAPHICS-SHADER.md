# Actual graphics shader translation

`GraphicsShader::compile` calls the real owned shader C ABI with explicit AIR
disassembler and SPIR-V validator paths. It retains the compiler result and an
independent immutable copy of the actual SPIR-V, exact translated entry point,
Metal entry name, stage and reflection. There is no environment fallback,
fixture substitution, device construction or GPU operation.

The current profile accepts vertex or fragment stages only. Bounded reflection
version 56 admission requires no bindings, attributes, varyings, constants,
tessellation, imageblocks, depth or stencil output. Vertex stages must write
position without InstanceIndex; VertexIndex is optional. Fragment stages must
write exactly one `float4` color at location zero. Unknown schema fields fail
closed. The immutable stage still needs real NGG/PS compilation and independent
machine-code admission before it can become a graphics pipeline.

`HNMetalLibrary` selects the function's actual metallib stage. Its public factory
can create a real vertex, fragment or compute `HNMetalFunction`, with the matching
`functionType` and native shader owner. The existing internal compute-only factory
continues to reject graphics stages. The compute Shader implementation is unchanged.

`tests/check_graphics_shader.py` uses the actual captured Apple AIR and metallib,
checks byte identity against prior SPIR-V, tests ownership after input destruction,
mutates actual reflection across resource/type/schema/truncation boundaries and
rejects the real SDL varying fragment. It runs strict and UBSan host tests plus
a Darwin C++ compile. Objective-C compilation is part of the next native frontend
snapshot; these host tests do not instantiate any Metal object.

The genuine Apple vertex output preserves Metal clip coordinates. In particular,
it does not negate clip Y. A Metal draw must therefore map NDC Y=+1 to the top of
the viewport using a negative hardware Y scale exactly once. The generic shader
translator and captured SPIR-V are not rewritten to compensate for a viewport.
