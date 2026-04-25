/*
 * WorldRenderer_linux_stub.cpp - headless no-op stub for Linux builds
 *
 * The full WorldRenderer (src/vm/WorldRenderer.cpp) renders the Smalltalk
 * World morph tree to a pixel buffer using CoreText / CoreGraphics.  Linux
 * builds are headless (test_load_image / SUnit) and never need to render
 * a frame — but Interpreter.cpp still constructs a WorldRenderer member.
 * This stub satisfies the linker without pulling in any Apple frameworks.
 *
 * If/when a Linux GUI is added, this file will be replaced with a real
 * implementation that talks to FreeType + cairo (already xcframework deps
 * on Apple, so the rendering math can be shared).
 */

#include "WorldRenderer.hpp"
#include "ObjectMemory.hpp"

namespace pharo {

WorldRenderer::WorldRenderer(ObjectMemory& memory) : memory_(memory) {}

void WorldRenderer::render() {}

std::string WorldRenderer::getMorphClassName(Oop /*morph*/) const { return {}; }

uint32_t WorldRenderer::extractColor(Oop /*colorObj*/) const { return 0; }

bool WorldRenderer::extractRect(Oop /*rect*/, int& /*x1*/, int& /*y1*/,
                                int& /*x2*/, int& /*y2*/) const { return false; }

bool WorldRenderer::extractBounds(Oop /*morph*/, int /*dispWidth*/,
                                  int /*dispHeight*/, int& /*x1*/,
                                  int& /*y1*/, int& /*x2*/,
                                  int& /*y2*/) const { return false; }

std::string WorldRenderer::extractString(Oop /*strObj*/) const { return {}; }

void WorldRenderer::renderMenuBar(uint32_t* /*pixels*/, int /*dispWidth*/,
                                  int /*dispHeight*/, Oop /*menubarMorph*/,
                                  int /*menuBarHeight*/) {}

void WorldRenderer::renderMorph(uint32_t* /*pixels*/, int /*dispWidth*/,
                                int /*dispHeight*/, Oop /*morph*/,
                                int /*depth*/, int /*index*/,
                                int& /*totalMorphsDrawn*/) {}

void WorldRenderer::renderWorldMenuOverlay(uint32_t* /*pixels*/,
                                           int /*dispWidth*/,
                                           int /*dispHeight*/) {}

}  // namespace pharo
