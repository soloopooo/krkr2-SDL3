#pragma once
// Stub replacing cocos2d-x's renderer/ccGLStateCache.h for SDL2-only build.
#include "cocos2d.h"

#ifndef CHECK_GL_ERROR_DEBUG
#define CHECK_GL_ERROR_DEBUG()
#endif
#ifndef GL_DEPTH24_STENCIL8
#define GL_DEPTH24_STENCIL8 0x88F0
#endif

// EVENT_RENDERER_RECREATED used without cocos2d:: prefix in RenderManager_ogl.cpp
static const std::string EVENT_RENDERER_RECREATED = "director_renderer_recreated";

NS_CC_BEGIN
namespace GL {
    inline void activeTexture(GLenum texture) { glActiveTexture(texture); }
    inline void bindTexture2D(GLuint textureId) { glBindTexture(GL_TEXTURE_2D, textureId); }
    inline void bindTexture2DN(GLuint textureUnit, GLuint textureId) {
        glActiveTexture(GL_TEXTURE0 + textureUnit);
        glBindTexture(GL_TEXTURE_2D, textureId);
    }
    inline void deleteTexture(GLuint textureId) {
        if (textureId) glDeleteTextures(1, &textureId);
    }
    inline void useProgram(GLuint program) { glUseProgram(program); }
    inline void blendResetToCache() {
        glBlendEquation(GL_FUNC_ADD);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glEnable(GL_BLEND);
    }
    inline void enableVertexAttribs(uint32_t flags) {
        for (int i = 0; i < 16; i++) {
            if (flags & (1u << i))
                glEnableVertexAttribArray(i);
            else
                glDisableVertexAttribArray(i);
        }
    }
}
NS_CC_END
