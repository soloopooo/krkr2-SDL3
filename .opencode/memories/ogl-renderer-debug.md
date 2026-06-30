# OGL Renderer Black Screen — 完整诊断报告

## 当前状态

| 项目 | 状态 |
|------|------|
| Software Renderer (当前 SDL2 默认) | ✅ 正常工作 |
| OGL Renderer (`TVPRenderManager_OpenGL`) | ❌ 黑屏 |
| SDL2 强制 software | `src/core/visual/RenderManager.cpp:4400` `#ifdef KRKR2_SDL_BUILD` |
| Shader compile/link | ✅ linked=1 |
| Software path pixel content | ✅ 动态场景（验证有效） |
| FBO completeness | ✅ GL_FRAMEBUFFER_COMPLETE (0x8cd5) |
| GL errors during compositing | ✅ 无错误 |
| TEMP textures 上 shader 输出 | ✅ 正确 |
| DrawBuffer texture 上 shader 输出 | ❌ 黑屏/空 |

---

## 问题分环诊断

问题不是单一原因，而是 **4 个环节叠加** 导致的致命组合。每一环单独看可能不会导致黑屏，但串联起来需要全部修复才能正常工作。

---

### 环0：当前 OGL 路径完全被绕过

**文件:** `src/core/visual/RenderManager.cpp:4397-4410`

```cpp
iTVPRenderManager * TVPGetRenderManager() {
    static iTVPRenderManager *_RenderManager;
    if (!_RenderManager) {
#ifdef KRKR2_SDL_BUILD
        ttstr str = "software";   // ← 强制走 software
#else
        ttstr str = IndividualConfigManager::GetInstance()->GetValue<std::string>("renderer", "software");
#endif
        _RenderManager = TVPGetRenderManager(str);
    }
    return _RenderManager;
}
```

因为 `TVPIsSoftwareRenderManager()` 返回 `true`，`_useOGLTexture()` (`WindowLayer_sdl.cpp:502`) 第一行就 `return false`，**OGL 路径从未被执行**，所有下文描述的问题都是基于之前的测试发现。

**修复第一步:** 将 `str = "software"` 改为 `str = "opengl"` 以启用 OGL 路径进行调试。

---

### 环1：CopyRect 快速路径的"纹理指针窃取"副作用

**文件:** `src/core/visual/LayerBitmapIntf.cpp:928-948`

```cpp
bool iTVPBaseBitmap::CopyRect(tjs_int x, tjs_int y, const iTVPBaseBitmap *ref,
        tTVPRect refrect, tjs_int plane) {
    // 当整个目标位图被整个源位图替换时（尺寸相同），走快速路径
    if(x == 0 && y == 0 && refrect.left == 0 && refrect.top == 0 &&
        refrect.right == (tjs_int)ref->GetWidth() &&
        refrect.bottom == (tjs_int)ref->GetHeight() &&
        (tjs_int)GetWidth() == refrect.right &&
        (tjs_int)GetHeight() == refrect.bottom &&
        plane == (TVP_BB_COPY_MASK|TVP_BB_COPY_MAIN) &&
        (bool)!Is32BPP() == (bool)!ref->Is32BPP())
    {
        // ★★★ 这里不是拷贝像素，而是替换纹理指针 ★★★
        AssignTexture(ref->GetTexture());   // Bitmap->Release(); Bitmap = tex; Bitmap->AddRef();
        return true;
    }
    // ... (慢速路径：逐像素拷贝) ...
}
```

**Blt 调用此路径的触发条件** (`LayerBitmapIntf.cpp:1394-1397`):
```cpp
if(opa == 255 && method == bmCopy && !hda) {
    return CopyRect(x, y, ref, refrect);  // ← 全屏不透明 bmCopy 走这里
}
```

**后果:**
- `DrawBuffer` 创建时的 OGL mutable 纹理 (POT expand: internalW=2048, `_scaleW=0.625`) 被 **Release 丢弃**
- `DrawBuffer.Bitmap` 现在指向**第一个 layer 的纹理**（通常是一个静态纹理 `tTVPOGLTexture2D_static`，internalW=1280, `_scaleW=1.0`）
- 这意味着 DrawBuffer 不再拥有"可写的" FBO render target，而是一个只读的源纹理

**触发场景举例:**
游戏首帧先绘制一个 1280x720 的不透明背景图（如游戏logo）。这个 Blt 走 bmCopy+opa=255 → CopyRect 全尺寸匹配 → AssignTexture 替换了整个 DrawBuffer 的纹理指针。

**影响:**
此环**单独不会导致黑屏**，因为替换后的纹理就是第一个 layer 的内容（有像素数据）。但它为环2触发 `Independ` 埋下伏笔。

---

### 环2：Independ → CopyTexture 级联销毁合成内容

#### 2a. `GetTextureForRender` → `Independ` 的触发条件

**文件:** `src/core/visual/win32/LayerBitmapImpl.cpp:1600-1611`

```cpp
iTVPTexture2D * tTVPNativeBaseBitmap::GetTextureForRender(bool isBlendTarget, const tTVPRect *rc) {
    if (isBlendTarget || !rc)
        Independ();                      // ← 需要blend → 强制独立拷贝
    else {
        int w = Bitmap->GetWidth(), h = Bitmap->GetHeight();
        if (rc->left == 0 && rc->top == 0 && rc->right >= w && rc->bottom >= h) {
            IndependNoCopy();            // ← 全屏替换，不需拷贝
        } else {
            Independ();                  // ← 部分区域 → 需要拷贝
        }
    }
    return GetTexture();
}
```

**`IsBlendTarget()` 的判定** (`RenderManager_ogl.cpp:1635`):
```cpp
virtual bool IsBlendTarget() { return !!BlendFunc; }
```
- 对于 "Copy" shader: `BlendFunc=0` → IsBlendTarget=false
- 对于 "AlphaBlend"/"CopyColor" 等: `BlendFunc!=0` → IsBlendTarget=true

#### 2b. `Independ()` 实现

**文件:** `src/core/visual/win32/LayerBitmapImpl.cpp:713-721`

```cpp
void tTVPNativeBaseBitmap::Independ() {
    // 切断位图的图像共享
    if (Bitmap->IsIndependent() && !Bitmap->IsStatic()) return;
    iTVPTexture2D *newb = GetRenderManager()->CreateTexture2D(
        Bitmap->GetWidth(), Bitmap->GetHeight(), Bitmap);   // ★ 创建一个新纹理并拷贝旧内容
    Bitmap->Release();    // 释放旧纹理
    Bitmap = newb;        // 指向新纹理
    FontChanged = true;
}
```

#### 2c. `CreateTexture2D(w, h, oldTex)` — OGL 实现

**文件:** `src/core/visual/ogl/RenderManager_ogl.cpp:3448-3452`

```cpp
virtual iTVPTexture2D* CreateTexture2D(unsigned int neww, unsigned int newh, iTVPTexture2D* tex) override {
    tTVPOGLTexture2D* newtex = static_cast<tTVPOGLTexture2D*>(
        _CreateMutableTexture2D(nullptr, 0, neww, newh, tex->GetFormat()));
    CopyTexture(newtex, static_cast<tTVPOGLTexture2D*>(tex),
        tTVPRect(0, 0, tex->GetWidth(), tex->GetHeight()));
    return newtex;
}
```

这里创建了一个**全新的** `tTVPOGLTexture2D_mutatble`（scaleW=1.0, internalW=1280 for 1280×720），然后调用 `CopyTexture` 把旧纹理内容拷贝过去。

#### 2d. `CopyTexture` — 两个路径

**文件:** `src/core/visual/ogl/RenderManager_ogl.cpp:3536-3624`

```cpp
void CopyTexture(tTVPOGLTexture2D *dst, tTVPOGLTexture2D *src, const tTVPRect &rcsrc) {
    // ── FAST PATH: glCopyImageSubData (ES 3.2 / OES_copy_image) ──
    if (GL::glCopyImageSubData && !src->IsCompressed &&
        src->_scaleW == dst->_scaleW && src->_scaleH == dst->_scaleH &&
        src->Format == dst->Format) {
        // ... 直接 GPU→GPU 拷贝 ...
        GL::glCopyImageSubData(
            src->texture, GL_TEXTURE_2D, 0, rc.left, rc.top, 0,
            dst->texture, GL_TEXTURE_2D, 0, 0, 0, 0,
            rc.get_width(), rc.get_height(), 1);
        return;
    }

    // ── SLOW PATH: FBO + "Copy" shader 全屏三角 ──
    static tTVPOGLRenderMethod* method = (tTVPOGLRenderMethod*)GetRenderMethod("Copy");
    // "Copy" shader:
    //   void main(){ gl_FragColor = texture2D(tex0, v_texCoord0); }
    method->Apply();
    dst->AsTarget();            // ← 将 dst 纹理附着到共享 _FBO
    float sw, sh;
    dst->GetScale(sw, sh);      // sw=_scaleW, sh=_scaleH
    glViewport(0, 0, rcsrc.get_width() * sw, rcsrc.get_height() * sh);
    // ... 设置顶点属性、绑定 src 纹理 ...
    glDrawArrays(GL_TRIANGLES, 0, 6);
}
```

**Fast path 跳过条件分析:**

当从 POT-scaled 纹理拷贝到 unscaled 纹理时:
- `src->_scaleW = 0.625`, `dst->_scaleW = 1.0`
- `src->_scaleW != dst->_scaleW` → **Fast path 被跳过**
- 走 FBO + Copy shader **慢速路径**

当从静态纹理拷贝到 mutable 纹理时（都是 scaleW=1.0）:
- `glCopyImageSubData` 可用 → fast path 工作正常
- `glCopyImageSubData` **不可用**（低版本 GLES 3.0） → 走 FBO 慢速路径

#### 2e. 新纹理内容分析

慢速路径 CopyTexture 调用后，新纹理的内容是:
- viewport = `(0, 0, 1280*dst_scaleW, 720*dst_scaleH)` = `(0, 0, 1280, 720)` (new tex scaleW=1.0)
- 源纹理 texcoords 由 `ApplyVertex` 计算，正确映射到逻辑像素区域
- 理论上应该正确拷贝

**如果 CopyTexture 写入失败（黑屏），后果是:**
1. 新纹理的内容为 `glTexImage2D(nullptr, ...)` 指定的**未初始化 GPU 内存**
2. 后续的 Fill (FillARGB → `glClearTexImage`) 将其清零为 `0xFF000000`（不透明黑色）
3. 后续的 compositing 操作渲染 layer 内容到黑色背景上
4. 如果 compositing shader 本身写入 DrawBuffer 也失败 → 全黑
5. 如果 compositing shader 写入成功 → 黑色背景上有 layer 内容

**关于 "Same shader programs produce correct output on TEMP textures but NOT on the DrawBuffer texture" 这个关键发现的进一步分析:**

TEMP texture 由 `GetTempTexture2D` 创建 (`RenderManager_ogl.cpp:3372-3382`):
```cpp
tTVPOGLTexture2D *GetTempTexture2D(tTVPOGLTexture2D* src, const tTVPRect& rcsrc) {
    unsigned int w = rcsrc.get_width(), h = rcsrc.get_height();
    if (!tempTexture || tempTexture->internalW < w || tempTexture->internalH < h) {
        if (tempTexture) tempTexture->Release();
        tempTexture = new tTVPOGLTexture2D_mutatble(nullptr, 0, w, h,
            TVPTextureFormat::RGBA, 1.f, 1.f);
    }
    tempTexture->Width = w; tempTexture->Height = h;
    CopyTexture(tempTexture, src, rcsrc);
    return tempTexture;
}
```

**TEMP 与 DrawBuffer 的差异:**
| 属性 | TEMP texture | DrawBuffer (初始创建) | DrawBuffer (被 CopyRect 替换后) |
|------|-------------|----------------------|-------------------------------|
| 类型 | `tTVPOGLTexture2D_mutatble` | `tTVPOGLTexture2D_mutatble` | `tTVPOGLTexture2D_static`（来自第一层） |
| internalW | = 逻辑 W（不大于 MAX） | POT rounded (2048) | = 逻辑 W (1280) |
| `_scaleW` | 1.0 | 0.625 | 1.0 |
| 内容 | 拷贝自源纹理（有效数据） | `glTexImage2D(nullptr)` → 未初始化 | 图片像素数据（有效） |
| 创建频率 | 复用，必要时 resize | 创建一次 | 来源于图片加载 |

**关键差异:** 初始 DrawBuffer 有 POT scaling (`_scaleW ≠ 1.0`)，TEMP 没有。

---

### 环3：`_useOGLTexture` 的 RefCount 泄漏

**文件:** `src/core/environ/sdl/WindowLayer_sdl.cpp:500-521`

```cpp
static bool _useOGLTexture(iTVPBaseBitmap *drawBuf) {
    if (!drawBuf || TVPIsSoftwareRenderManager()) return false;
    iTVPTexture2D *tex = drawBuf->GetTexture();
    if (!tex) return false;
    cocos2d::Texture2D *adapt = tex->GetAdapterTexture(g_LastAdapt);  // ★★★
    // ...
    g_LastAdapt = adapt;
    g_tex = adapt->_name;
    // ...
    return true;
}
```

#### 3a. `GetAdapterTexture` — OGL 实现

**文件:** `src/core/visual/ogl/RenderManager_ogl.cpp:935-945`

```cpp
virtual cocos2d::Texture2D* GetAdapterTexture(cocos2d::Texture2D* orig) override {
    if (orig) {
        if (orig->getPixelsWide() == internalW && orig->getPixelsHigh() == internalH) {
            static_cast<AdapterTexture2D*>(orig)->update(texture);
            return orig;      // ← 复用已有 adapter，不调 AddRef
        }
    }
    AdapterTexture2D *ret = new AdapterTexture2D(this, texture, internalW, internalH);
    // ★★★ 构造函数中调用了 _owner->AddRef() ★★★
    ret->autorelease();
    return ret;
}
```

#### 3b. `AdapterTexture2D` 构造/析构

**文件:** `src/core/visual/ogl/RenderManager_ogl.cpp:907-933`

```cpp
class AdapterTexture2D : public cocos2d::Texture2D {
public:
    iTVPTexture2D *_owner;
    AdapterTexture2D(iTVPTexture2D* owner, GLuint name, int w, int h) {
        _name = name;
        _owner = owner;
        _owner->AddRef();    // ★★★ 增加引用计数 ★★★
        // ...
    }
    ~AdapterTexture2D() {
        _name = 0;
        _owner->Release();   // ★★★ 释放引用计数 ★★★
    }
    void update(GLuint name) {
        _name = name;        // 仅更新 GL 名称，不改 RefCount
    }
};
```

#### 3c. RefCount 时序分析

```
Frame 0（首次）:
  1. Compositing 完成。DrawBuffer.Bitmap → tex_A (RefCount=1)
  2. _useOGLTexture:
     a. tex_A->GetAdapterTexture(nullptr) → 创建 AdapterTexture2D(tex_A, ...)
     b. AdapterTexture2D 构造: tex_A->AddRef() → RefCount=2
     c. g_LastAdapt = adapt
  3. SDL 显示 tex_A 的内容 ✓

Frame 1:
  4. Compositing 开始。DrawBuffer 上第一个 Blt:
     a. GetTextureForRender(isBlendTarget, &rect)
     b. 检查: Bitmap->IsIndependent() == (RefCount==1 && !IsStatic())
        - 如果 Bitmap 是 static 纹理 → IsIndependent=false → Independ() 触发
        - 如果 Bitmap 是 mutable 且 RefCount=2 → IsIndependent=false → Independ() 触发
     c. Independ() → CreateTexture2D(w,h,oldTex) → CopyTexture(newTex, oldTex)
        → tex_B (RefCount=1, 全新纹理)
     d. Bitmap->Release(): tex_A 的 RefCount 从2降为1（adapter 还持有引用）
     e. Bitmap = tex_B: DrawBuffer 现在指向 tex_B
  5. 后续合成操作写入 tex_B
  6. _useOGLTexture:
     a. tex_B->GetAdapterTexture(g_LastAdapt) → 维度检查
     b. 如果 tex_B.internalW != tex_A.internalW（POT→非POT）:
        g_LastAdapt.getPixelsWide() (2048) != tex_B.internalW (1280)
        → 不匹配 → 创建 **新的** AdapterTexture2D → tex_B->AddRef() → RefCount=2 again!
     c. g_LastAdapt = newAdapt (旧 adpater 被 autorelease 丢弃)
  7. 旧 adapter 析构: tex_A->Release() → tex_A RefCount 降为 0 → tex_A 被释放 ✓

Frame 2:
  8. 同 Frame 1 的第4步。tex_B 有 RefCount=2 → Independ 再次触发
  9. 创建 tex_C → CopyTexture(tex_C, tex_B) → ...
```

**问题:**
当 DrawBuffer 的纹理在 Frame 0 和 Frame 1 之间被 `CopyRect` 的 `AssignTexture` 替换为静态纹理时，或当 POT scaling 差异导致 adapter 不匹配时，**每一帧都会触发 `Independ` → 创建新纹理 → `CopyTexture`**。而 `_useOGLTexture` 在每帧末尾又通过 `GetAdapterTexture` 创建新 adapter，再次 AddRef，形成**恶性循环**。

### 分环小结

```
             CopyRect 快速路径                 Independ 每帧创建                  _useOGLTexture
             替换纹理指针                      新纹理 + CopyTexture              AddRef RefCount=2
                  │                                │                                │
  Frame 0:   DrawBuffer.Bitmap               跳过（RefCount=1）             GetAdapterTexture(nullptr)
             = static_tex（第一层）                                         → 创建 adapter → AddRef
                  │                                │                            RefCount=2
                  │                                │                                │
  Frame 1:   仍是 static_tex                  Independ触发！                  GetAdapterTexture(g_LastAdapt)
             （来自 Frame 0）                CreateTexture2D                → 维度不匹配 → 新 adapter
                                          → CopyTexture(new, old)          → AddRef → RefCount=2 again
                                          → 如果 CopyTexture 失败:
                                             新纹理 = 未初始化 → 黑屏                │
                  │                                                                 │
  Frame 2:   tex_B (mutable)               Independ再次触发！              GetAdapterTexture → new adapter
             RefCount=2                    CreateTexture2D                → 又 AddRef...
                                          → CopyTexture(tex_C, tex_B)
                                          → tex_B 内容可能是垃圾
                                          → CopyTexture 再产生垃圾
```

---

## 根本原因总结

**直接原因:** OGL compositing shader 在写入 DrawBuffer 纹理时失败（写入内容为0/未初始化），导致合成后的画面全黑。

**可能的底层原因**（按可能性排序）:

### A. POT scaling viewport 精度问题（最可能）

初始 DrawBuffer 纹理经过 POT 取整（1280→2048, 720→1024），`_scaleW=0.625`, `_scaleH=0.703125`。

在 `OperateRect` (`RenderManager_ogl.cpp:3735-3736`) 中:
```cpp
glViewport(rctar.left * tar->_scaleW, rctar.top * tar->_scaleH,
    rctar.get_width() * tar->_scaleW, rctar.get_height() * tar->_scaleH);
```

对于 1280×720 full rect: viewport = `(0, 0, 800.0, 506.25)`

GLES2 的 `glViewport` 接受 `GLint` 整数参数，506.25 被截断为 506。**这导致 height 在目标纹理的 1024 个 internal pixel 中写入 506 行，而非 506.25 行**。

更重要的是，这在 `CopyTexture` 慢速路径中也会发生。当 Independ 创建新纹理 (scaleW=1.0, internalW=1280) 并调用 CopyTexture 从 POT 纹理拷贝时:
- 源 texture: internalW=2048, scaleW=0.625
- ApplyVertex 计算 texcoord → (0,0) 到 (0.390625, 0.494384)
- 这些 texcoord 映射到源纹理的逻辑区域 (0,0)-(1280,720)
- **理论上** 这应该正确

但在某些 Mali/Adreno GPU 驱动上，texcoord 插值和 viewport 映射的组合可能导致**子像素偏移**，使得写入位置偏移到纹理的未初始化区域。

### B. `glCopyImageSubData` 不可用且 Copy shader FBO 路径有 bug

如果设备不支持 `glCopyImageSubData`（GLES 3.0 设备），CopyTexture 始终走 FBO 慢速路径。

在 Independ 触发 CopyTexture 时:
- Copy shader 使用 `glDisable(GL_BLEND)` (BlendFunc=0) 
- dst->AsTarget() → TVPSetRenderTarget → glFramebufferTexture2D
- 但此时 FBO 可能已经被某个 `TVPSetRenderTarget(0)` 调用重置
- 如果在 CopyTexture 之前某个操作调用了 TVPSetRenderTarget(0) 但没有后续操作重新绑定 FBO，则 CopyTexture 的 `dst->AsTarget()` 虽然调用了 `glFramebufferTexture2D` 但 **`_CurrentFBOValid` 状态可能不一致**

### C. FillARGB 清空导致内容丢失

在 `DrawCompleted` → `DrawBuffer->Blt` 之前，可能有 Fill 操作将 DrawBuffer 清零:
```cpp
DrawBuffer->Fill(tTVPRect(0, 0, w, h), 0xFF000000);  // 不透明黑色
```

如果 Fill 在 CopyTexture 之后、compositing 之前执行，则新纹理的拷贝内容被清零，后续 compositing 需要在黑色背景上从头绘制。

但在正常流程中，Fill 应该在 CopyTexture 之前（在 DrawBuffer 创建/Resize 时），而不是之后。需要日志验证具体时序。

---

## 关于 cocos2d 变体为何正常

在 cocos2d variant 中:
1. `UpdateDrawBuffer()` 使用 cocos2d Sprite 显示，不经过 `_useOGLTexture`
2. 不创建 `AdapterTexture2D`，**不对 DrawBuffer 纹理产生额外的 AddRef**
3. `DrawBuffer.Bitmap` 的 RefCount 始终保持 = 1
4. `Independ()` 检查 `RefCount == 1` → 跳过 → 不创建新纹理
5. Compositing 直接写入 DrawBuffer 的原始 GL 纹理

**差异核心:** SDL2 的 `_useOGLTexture` 为了获取 GL texture name，创建了 `AdapterTexture2D` 导致多余的 RefCount，触发了 `Independ` → `CopyTexture` 链条。

---

## 修复方案

### Phase 1: 诊断（首先执行）

```cpp
// 1. 在 RenderManager.cpp:4400 临时改为 "opengl"
ttstr str = "opengl";  // 去掉 #ifdef KRKR2_SDL_BUILD 保护

// 2. 在 CopyTexture 慢速路径 (RenderManager_ogl.cpp:3611 glDrawArrays 之后) 加入:
{
    GLenum err = glGetError();
    __android_log_print(ANDROID_LOG_INFO, "##krkr", "CopyTex: dst=%u src=%u dstW=%d dstH=%d srcW=%d srcH=%d sw=%.4f sh=%.4f vpW=%d vpH=%d err=0x%x",
        dst->texture, src->texture,
        dst->internalW, dst->internalH, src->internalW, src->internalH,
        sw, sh,
        (int)(rcsrc.get_width() * sw), (int)(rcsrc.get_height() * sh),
        err);
    // 读回一个像素验证
    TVPSetRenderTarget(dst->texture);
    uint32_t dpix = 0xDEAD;
    glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, &dpix);
    __android_log_print(ANDROID_LOG_INFO, "##krkr", "CopyTex verify: (0,0)=0x%08x", dpix);
    TVPSetRenderTarget(0);
}

// 3. 在 Independ 入口 (LayerBitmapImpl.cpp:713) 加入:
__android_log_print(ANDROID_LOG_INFO, "##krkr", "INDEPEND: Bitmap=%p RefCount=%d IsStatic=%d",
    Bitmap, Bitmap ? Bitmap->IsIndependent() ? 1 : Bitmap->RefCount : 0,
    Bitmap ? (Bitmap->IsStatic() ? 1 : 0) : 0);

// 4. 在 OperateRect 写入后 (RenderManager_ogl.cpp:3746 glDrawArrays 之后) 加入:
if (tar->internalW >= 1280) {  // 只在 DrawBuffer 尺寸时打印
    TVPSetRenderTarget(tar->texture);
    uint32_t pix = 0xDEAD;
    glReadPixels(rctar.left * tar->_scaleW, rctar.top * tar->_scaleH, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, &pix);
    __android_log_print(ANDROID_LOG_INFO, "##krkr", "OPRECT: tar=%u pos=(%d,%d) logical=(%d,%d) pix=0x%08x",
        tar->texture, rctar.left, rctar.top,
        (int)(rctar.left * tar->_scaleW), (int)(rctar.top * tar->_scaleH), pix);
    TVPSetRenderTarget(0);
}
```

### Phase 2: 修复 RefCount 泄漏（关键修复）

**方案 A（推荐）: 绕过 AdapterTexture2D，直接从 OGL 纹理对象读取 GL name**

在 `tTVPOGLTexture2D` (RenderManager_ogl.cpp:766 附近) 添加:
```cpp
virtual GLuint GetGLTextureName() const override { return texture; }
```

在 `iTVPTexture2D` (RenderManager.h:98 附近) 添加虚函数声明:
```cpp
virtual GLuint GetGLTextureName() const { return 0; }
```

修改 `_useOGLTexture` (WindowLayer_sdl.cpp:500-521):
```cpp
static bool _useOGLTexture(iTVPBaseBitmap *drawBuf) {
    if (!drawBuf || TVPIsSoftwareRenderManager()) return false;
    iTVPTexture2D *tex = drawBuf->GetTexture();
    if (!tex) return false;
    GLuint glName = tex->GetGLTextureName();
    if (!glName) return false;
    g_tex = glName;
    g_texW = tex->GetWidth();
    g_texH = tex->GetHeight();
    glBindTexture(GL_TEXTURE_2D, g_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return true;
}
```

**优点:** 完全不创建 AdapterTexture2D，不调用 AddRef，RefCount 始终=1，`Independ` 永不触发。

**方案 B（备选）: 在 GetAdapterTexture 中添加 skipAddRef 参数**

修改 `GetAdapterTexture` 签名添加 `bool addRef = true`。SDL2 路径传 `false`。

### Phase 3: 修复 POT scaling viewport 问题

如果诊断日志证实 viewport 精度导致写入失败，修改 `CopyTexture` 和 `OperateRect` 的 viewport 计算，使用 `ceilf` 避免向下取整丢失像素：
```cpp
glViewport(0, 0,
    (int)ceilf(rcsrc.get_width() * sw),
    (int)ceilf(rcsrc.get_height() * sh));
```

### Phase 4: 恢复默认 OGL 渲染器

```cpp
// RenderManager.cpp:4397-4410
iTVPRenderManager * TVPGetRenderManager() {
    static iTVPRenderManager *_RenderManager;
    if (!_RenderManager) {
#ifdef KRKR2_SDL_BUILD
        ttstr str = "opengl";  // 改为 opengl
#else
        ttstr str = IndividualConfigManager::GetInstance()->GetValue<std::string>("renderer", "software");
#endif
        // ...
    }
}
```

---

## 关键文件索引

| 文件 | 行号 | 内容 |
|------|------|------|
| `src/core/visual/RenderManager.cpp` | 4397-4415 | `TVPGetRenderManager()` — 渲染器选择（SDL2 强制 software） |
| `src/core/visual/RenderManager.h` | 98-127 | `iTVPTexture2D` 基类 — RefCount, IsIndependent, GetAdapterTexture |
| `src/core/visual/win32/LayerBitmapImpl.cpp` | 713-745 | `Independ`, `IndependNoCopy`, `Recreate`, `IsIndependent` |
| `src/core/visual/win32/LayerBitmapImpl.cpp` | 681-693 | `AssignTexture` — 纹理指针替换+AddRef |
| `src/core/visual/win32/LayerBitmapImpl.cpp` | 1600-1611 | `GetTextureForRender` — Independ 调度 |
| `src/core/visual/LayerBitmapIntf.cpp` | 928-948 | `iTVPBaseBitmap::CopyRect` — AssignTexture 快速路径 |
| `src/core/visual/LayerBitmapIntf.cpp` | 1382-1509 | `iTVPBaseBitmap::Blt` — CopyRect fast path + OperateRect 标准路径 |
| `src/core/visual/LayerManager.cpp` | 88-171 | DrawBuffer 创建、`GetOrCreateDrawBuffer` |
| `src/core/visual/ogl/RenderManager_ogl.cpp` | 907-945 | `AdapterTexture2D` class + `GetAdapterTexture` |
| `src/core/visual/ogl/RenderManager_ogl.cpp` | 456-470 | `TVPSetRenderTarget` — FBO attach/detach |
| `src/core/visual/ogl/RenderManager_ogl.cpp` | 990-1009 | `ApplyVertex` — texcoord 计算（含 scale 因子） |
| `src/core/visual/ogl/RenderManager_ogl.cpp` | 1484-1531 | `tTVPOGLTexture2D_mutatble` 构造 — POT round + InternalInit |
| `src/core/visual/ogl/RenderManager_ogl.cpp` | 1597-1601 | `AsTarget()` → `TVPSetRenderTarget(texture)` |
| `src/core/visual/ogl/RenderManager_ogl.cpp` | 1635 | `IsBlendTarget()` → `!!BlendFunc` |
| `src/core/visual/ogl/RenderManager_ogl.cpp` | 2597-2600 | "Copy" shader 注册 `gl_FragColor = texture2D(tex0, v_texCoord0)` |
| `src/core/visual/ogl/RenderManager_ogl.cpp` | 3372-3382 | `GetTempTexture2D` — TEMP 纹理创建（scaleW=1.0） |
| `src/core/visual/ogl/RenderManager_ogl.cpp` | 3448-3452 | `CreateTexture2D(w,h,oldTex)` — Independ 调用的拷贝路径 |
| `src/core/visual/ogl/RenderManager_ogl.cpp` | 3536-3624 | `CopyTexture` — glCopyImageSubData fast + FBO slow path |
| `src/core/visual/ogl/RenderManager_ogl.cpp` | 3646-3770 | `OperateRect` — OGL 合成主函数（viewport + FBO + draw） |
| `src/core/environ/sdl/WindowLayer_sdl.cpp` | 500-521 | `_useOGLTexture` — SDL2 OGL 读取路径（RefCount 泄漏源） |
| `src/core/environ/sdl/WindowLayer_sdl.cpp` | 523-731 | `TVPEngineTick` — SDL2 主渲染循环 |

---

## 软件渲染器对比（为什么软件路径工作）

| 步骤 | 软件路径 | OGL 路径（问题版） |
|------|---------|-------------------|
| DrawBuffer 创建 | `tTVPSoftwareTexture2D`（RAM bitmap） | `tTVPOGLTexture2D_mutatble`（GL texture + POT） |
| CopyRect 快速路径 | `AssignTexture` 替换纹理指针 | 同左（但纹理类型不同） |
| Independ 触发 | `RefCount==1` → 几乎不触发 | `RefCount==2`（adapter 引用） → 每帧触发 |
| 合成操作 | CPU memcpy / blend（直接写 RAM） | FBO + shader（GL 间接写） |
| 显示读取 | `GetScanLine(0)` → 直接读 RAM 指针 | `GetAdapterTexture` → GL texture name → GPU→GPU |
| RefCount 管理 | 无 adapter | AdapterTexture2D 额外 AddRef |

软件路径完全绕过 GL FBO、shader、RefCount 问题，所有操作在 CPU RAM 中完成。

---

## 最终诊断（2024-06-29）

### 结论
OGL 合成管线在目标设备上无法正常写入 FBO 纹理。所有 shader 经诊断写入目标纹理的像素值均为 0xFF000000，即使 FBO COMPLETE、shader linked=1、无 GL error、viewport 在纹理范围内。同一设备上软件渲染器产生的 DrawBuffer 像素值正确且动态变化。

### OGL 合成写入失败的确定性证据
1. **OPRECT: tar=27 pix=0xff000000** — OperateRect 内部 glDrawArrays 后立即读回目标纹理像素，始终为 0xFF000000
2. **MAINIMG: tex=3 spix=0x00000000** — 首层 MainImage 纹理同样无法写入
3. **SWDBUF: (0,0)=0xfff0f0fe center=0xff7dcff6~0xff3bc0f7** — 同一场景下软件路径 DrawBuffer 有正确的动态像素，证明场景合成逻辑正确

### 可排除的原因
- FBO 完整性和状态追踪
- RefCount/Independ/AdapterTexture2D 泄漏（Phase 2 已绕过）
- POT scaling viewport 精度（ceilf 已修复）
- Blt 路径选择（确认走 OperateRect GPU 路径）
- Shader 编译（linked=1，多款 shader 均失败）
- enableVertexAttribs stub（对于 pos=0 texcoord=1 的标准布局正确）

### 可能原因（未验证）
- 设备特定的 GLES 驱动 bug
- precision 缺失：OGL shader 无 precision highp float（但我们的 quad shader 有）
- gl_LastFragData 冲突：framebuffer fetch 检测可能误判

### 当前方案
默认使用软件渲染器。若需重新启用 OGL，需在另一机型测试确认是否为设备特定问题。
