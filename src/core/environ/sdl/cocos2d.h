#pragma once
// Minimal stub replacing cocos2d-x's cocos2d.h for SDL2-only build.
// Provides just enough types for core engine files to compile.

#include <string>
#include <vector>
#include <set>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <EGL/egl.h>

#define NS_CC_BEGIN namespace cocos2d {
#define NS_CC_END }
#define USING_NS_CC using namespace cocos2d;

NS_CC_BEGIN

	struct Size { 
	float width, height; 
	Size() : width(0), height(0) {} 
	Size(float w, float h) : width(w), height(h) {} 
	static const Size ZERO;
};
struct Vec2 { float x, y; Vec2() : x(0), y(0) {} };
struct Rect { };

// Event types (minimal stubs for RenderManager_ogl)
class EventCustom {
public:
	EventCustom(const std::string&) {}
};
class EventDispatcher {
public:
	void addEventListenerWithFixedPriority(void*, int) {}
};
class EventListenerCustom {
public:
	template<typename Fn>
	static EventListenerCustom* create(const std::string&, Fn&&) { return nullptr; }
	void autorelease() {}
};

class Node {
public:
	Size _contentSize{0,0};
	virtual ~Node() {}
	static Node *create() { return new Node; }
	void addChild(Node*) {}
	void removeFromParent() {}
	void setVisible(bool) {}
	bool isVisible() { return true; }
	void setContentSize(const Size& s) { _contentSize = s; }
	void setPosition(const Vec2&) {}
	void setAnchorPoint(const Vec2&) {}
	void setScale(float) {}
	void runAction(void*) {}
	void stopAllActions() {}
	void setOpacity(uint8_t) {}
	void setColor(uint8_t, uint8_t, uint8_t) {}
	Size getContentSize() const { return _contentSize; }
};

class GLView {
public:
	Size getFrameSize() const { return Size(2048, 1200); }
};
class Director {
public:
	static Director *getInstance() { static Director d; return &d; }
	GLView *getOpenGLView() { static GLView v; return &v; }
	void setViewport() {}
	EventDispatcher* getEventDispatcher() { static EventDispatcher ed; return &ed; }
};

class FileUtils {
public:
	static FileUtils *getInstance() { static FileUtils f; return &f; }
	bool isFileExist(const std::string &path) const;
	std::string fullPathForFilename(const std::string &filename) const;
	std::string getStringFromFile(const std::string &path) const;
};

struct Data {
	const uint8_t *getBytes() const { return nullptr; }
	ssize_t getSize() const { return 0; }
};

class GLProgram;
class Texture2D : public Node {
public:
	GLuint _name = 0;
	int _pixelsWide = 0, _pixelsHigh = 0;
	float _maxS = 1.0f, _maxT = 1.0f;
	enum class PixelFormat { NONE = 0, AUTO = 1, BGRA8888 = 2, RGBA8888 = 3, RGB888 = 4, RGB565 = 5, A8 = 6, I8 = 7, AI88 = 8 };
	PixelFormat _pixelFormat = PixelFormat::RGBA8888;
	bool _hasPremultipliedAlpha = false;
	bool _hasMipmaps = false;

	static Texture2D *create() { return new Texture2D; }
	virtual void autorelease() {}
	void retain() {}
	void release() {}
	bool initWithData(const void*, ssize_t, int, int, int, int, const Size&) { return true; }
	bool initWithData(const void*, ssize_t, PixelFormat, int, int, const Size&) { return true; }
	bool initWithImage(void*) { return true; }
	void updateWithData(const void*, int, int, int, int) {}
	int getPixelsWide() const { return _pixelsWide; }
	int getPixelsHigh() const { return _pixelsHigh; }
	void setGLProgram(GLProgram*) {}
};

class Sprite : public Node {
public:
	static Sprite *create() { return new Sprite; }
	void setTexture(Texture2D*) {}
	void setTextureRect(const Rect&) {}
	void setAnchorPoint(const Vec2&) {}
	void setFlippedY(bool) {}
	void setBlendFunc(const void*) {}
};

NS_CC_END
