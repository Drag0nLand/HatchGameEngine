#if INTERFACE
#include <Engine/Includes/Standard.h>
#include <Engine/Includes/StandardSDL2.h>
#include <Engine/ResourceTypes/ISprite.h>
#include <Engine/ResourceTypes/IModel.h>
#include <Engine/Math/Matrix4x4.h>
#include <Engine/Math/Clipper.h>
#include <Engine/Rendering/Texture.h>
#include <Engine/Rendering/Material.h>
#include <Engine/Rendering/VertexBuffer.h>
#include <Engine/Rendering/Software/Contour.h>
#include <Engine/Rendering/Software/SoftwareEnums.h>
#include <Engine/Includes/HashMap.h>

class SoftwareRenderer {
public:
    static GraphicsFunctions BackendFunctions;
    static Uint32            CompareColor;
    static Sint32            CurrentPalette;
    static Uint32            PaletteColors[MAX_PALETTE_COUNT][0x100];
    static Uint8             PaletteIndexLines[MAX_FRAMEBUFFER_HEIGHT];
    static TileScanLine      TileScanLineBuffer[MAX_FRAMEBUFFER_HEIGHT];
    static Sint32            SpriteDeformBuffer[MAX_FRAMEBUFFER_HEIGHT];
    static bool              UseSpriteDeform;
    static Contour           ContourBuffer[MAX_FRAMEBUFFER_HEIGHT];
    static int               MultTable[0x10000];
    static int               MultTableInv[0x10000];
    static int               MultSubTable[0x10000];
};
#endif

#include <Engine/Rendering/Software/SoftwareRenderer.h>
#include <Engine/Rendering/Software/PolygonRasterizer.h>
#include <Engine/Rendering/Software/SoftwareEnums.h>
#include <Engine/Rendering/FaceInfo.h>
#include <Engine/Rendering/Scene3D.h>
#include <Engine/Rendering/PolygonRenderer.h>
#include <Engine/Rendering/ModelRenderer.h>

#include <Engine/Diagnostics/Log.h>
#include <Engine/Diagnostics/Memory.h>
#include <Engine/IO/ResourceStream.h>

#include <Engine/Bytecode/Types.h>
#include <Engine/Bytecode/BytecodeObjectManager.h>

GraphicsFunctions SoftwareRenderer::BackendFunctions;
Uint32            SoftwareRenderer::CompareColor = 0xFF000000U;
Sint32            SoftwareRenderer::CurrentPalette = -1;
Uint32            SoftwareRenderer::PaletteColors[MAX_PALETTE_COUNT][0x100];
Uint8             SoftwareRenderer::PaletteIndexLines[MAX_FRAMEBUFFER_HEIGHT];
TileScanLine      SoftwareRenderer::TileScanLineBuffer[MAX_FRAMEBUFFER_HEIGHT];
Sint32            SoftwareRenderer::SpriteDeformBuffer[MAX_FRAMEBUFFER_HEIGHT];
bool              SoftwareRenderer::UseSpriteDeform = false;
Contour           SoftwareRenderer::ContourBuffer[MAX_FRAMEBUFFER_HEIGHT];
int               SoftwareRenderer::MultTable[0x10000];
int               SoftwareRenderer::MultTableInv[0x10000];
int               SoftwareRenderer::MultSubTable[0x10000];

BlendState CurrentBlendState;

#if 0
Uint32 ColorAdd(Uint32 color1, Uint32 color2, int percent) {
	Uint32 r = (color1 & 0xFF0000U) + (((color2 & 0xFF0000U) * percent) >> 8);
	Uint32 g = (color1 & 0xFF00U) + (((color2 & 0xFF00U) * percent) >> 8);
	Uint32 b = (color1 & 0xFFU) + (((color2 & 0xFFU) * percent) >> 8);
    if (r > 0xFF0000U) r = 0xFF0000U;
	if (g > 0xFF00U) g = 0xFF00U;
	if (b > 0xFFU) b = 0xFFU;
	return r | g | b;
}
Uint32 ColorSubtract(Uint32 color1, Uint32 color2, int percent) {
    Sint32 r = (color1 & 0xFF0000U) - (((color2 & 0xFF0000U) * percent) >> 8);
	Sint32 g = (color1 & 0xFF00U) - (((color2 & 0xFF00U) * percent) >> 8);
	Sint32 b = (color1 & 0xFFU) - (((color2 & 0xFFU) * percent) >> 8);
    if (r < 0) r = 0;
	if (g < 0) g = 0;
	if (b < 0) b = 0;
	return r | g | b;
}
#endif

#define CLAMP_VAL(v, a, b) if (v < a) v = a; else if (v > b) v = b

Uint8 ColR;
Uint8 ColG;
Uint8 ColB;
Uint32 ColRGB;

Uint32 (*TintFunction)(Uint32*, Uint32*, Uint32, Uint32) = NULL;

#define TRIG_TABLE_BITS 11
#define TRIG_TABLE_SIZE (1 << TRIG_TABLE_BITS)
#define TRIG_TABLE_MASK ((1 << TRIG_TABLE_BITS) - 1)
#define TRIG_TABLE_HALF (TRIG_TABLE_SIZE >> 1)

int SinTable[TRIG_TABLE_SIZE];
int CosTable[TRIG_TABLE_SIZE];

PolygonRenderer polygonRenderer;

int FilterCurrent[0x8000];
int FilterInvert[0x8000];
int FilterBlackAndWhite[0x8000];

// Initialization and disposal functions
PUBLIC STATIC void     SoftwareRenderer::Init() {
    SoftwareRenderer::BackendFunctions.Init();

    UseSpriteDeform = false;
}
PUBLIC STATIC Uint32   SoftwareRenderer::GetWindowFlags() {
    return Graphics::Internal.GetWindowFlags();
}
PUBLIC STATIC void     SoftwareRenderer::SetGraphicsFunctions() {
    for (int alpha = 0; alpha < 0x100; alpha++) {
        for (int color = 0; color < 0x100; color++) {
            MultTable[alpha << 8 | color] = (alpha * color) >> 8;
            MultTableInv[alpha << 8 | color] = ((alpha ^ 0xFF) * color) >> 8;
            MultSubTable[alpha << 8 | color] = (alpha * -(color ^ 0xFF)) >> 8;
        }
    }

    for (int a = 0; a < TRIG_TABLE_SIZE; a++) {
        float ang = a * M_PI / TRIG_TABLE_HALF;
        SinTable[a] = (int)(Math::Sin(ang) * TRIG_TABLE_SIZE);
        CosTable[a] = (int)(Math::Cos(ang) * TRIG_TABLE_SIZE);
    }
    for (int a = 0; a < 0x8000; a++) {
        int r = (a >> 10) & 0x1F;
        int g = (a >> 5) & 0x1F;
        int b = (a) & 0x1F;

        int bw = ((r + g + b) / 3) << 3;
        int hex = r << 19 | g << 11 | b << 3;
        FilterBlackAndWhite[a] = bw << 16 | bw << 8 | bw | 0xFF000000U;
        FilterInvert[a] = (hex ^ 0xFFFFFF) | 0xFF000000U;
    }

    CurrentBlendState.Mode = BlendMode_NORMAL;
    CurrentBlendState.Opacity = 0xFF;
    CurrentBlendState.FilterTable = nullptr;

    SoftwareRenderer::CurrentPalette = 0;
    for (int p = 0; p < MAX_PALETTE_COUNT; p++) {
        for (int c = 0; c < 0x100; c++) {
            SoftwareRenderer::PaletteColors[p][c]  = 0xFF000000U;
            SoftwareRenderer::PaletteColors[p][c] |= (c & 0x07) << 5; // Red?
            SoftwareRenderer::PaletteColors[p][c] |= (c & 0x18) << 11; // Green?
            SoftwareRenderer::PaletteColors[p][c] |= (c & 0xE0) << 16; // Blue?
        }
    }
    memset(SoftwareRenderer::PaletteIndexLines, 0, sizeof(SoftwareRenderer::PaletteIndexLines));

    SoftwareRenderer::BackendFunctions.Init = SoftwareRenderer::Init;
    SoftwareRenderer::BackendFunctions.GetWindowFlags = SoftwareRenderer::GetWindowFlags;
    SoftwareRenderer::BackendFunctions.Dispose = SoftwareRenderer::Dispose;

    // Texture management functions
    SoftwareRenderer::BackendFunctions.CreateTexture = SoftwareRenderer::CreateTexture;
    SoftwareRenderer::BackendFunctions.LockTexture = SoftwareRenderer::LockTexture;
    SoftwareRenderer::BackendFunctions.UpdateTexture = SoftwareRenderer::UpdateTexture;
    // SoftwareRenderer::BackendFunctions.UpdateYUVTexture = SoftwareRenderer::UpdateTextureYUV;
    SoftwareRenderer::BackendFunctions.UnlockTexture = SoftwareRenderer::UnlockTexture;
    SoftwareRenderer::BackendFunctions.DisposeTexture = SoftwareRenderer::DisposeTexture;

    // Viewport and view-related functions
    SoftwareRenderer::BackendFunctions.SetRenderTarget = SoftwareRenderer::SetRenderTarget;
    SoftwareRenderer::BackendFunctions.UpdateWindowSize = SoftwareRenderer::UpdateWindowSize;
    SoftwareRenderer::BackendFunctions.UpdateViewport = SoftwareRenderer::UpdateViewport;
    SoftwareRenderer::BackendFunctions.UpdateClipRect = SoftwareRenderer::UpdateClipRect;
    SoftwareRenderer::BackendFunctions.UpdateOrtho = SoftwareRenderer::UpdateOrtho;
    SoftwareRenderer::BackendFunctions.UpdatePerspective = SoftwareRenderer::UpdatePerspective;
    SoftwareRenderer::BackendFunctions.UpdateProjectionMatrix = SoftwareRenderer::UpdateProjectionMatrix;
    SoftwareRenderer::BackendFunctions.MakePerspectiveMatrix = SoftwareRenderer::MakePerspectiveMatrix;

    // Shader-related functions
    SoftwareRenderer::BackendFunctions.UseShader = SoftwareRenderer::UseShader;
    SoftwareRenderer::BackendFunctions.SetUniformF = SoftwareRenderer::SetUniformF;
    SoftwareRenderer::BackendFunctions.SetUniformI = SoftwareRenderer::SetUniformI;
    SoftwareRenderer::BackendFunctions.SetUniformTexture = SoftwareRenderer::SetUniformTexture;

    // These guys
    SoftwareRenderer::BackendFunctions.Clear = SoftwareRenderer::Clear;
    SoftwareRenderer::BackendFunctions.Present = SoftwareRenderer::Present;

    // Draw mode setting functions
    SoftwareRenderer::BackendFunctions.SetBlendColor = SoftwareRenderer::SetBlendColor;
    SoftwareRenderer::BackendFunctions.SetBlendMode = SoftwareRenderer::SetBlendMode;
    SoftwareRenderer::BackendFunctions.SetTintColor = SoftwareRenderer::SetTintColor;
    SoftwareRenderer::BackendFunctions.SetTintMode = SoftwareRenderer::SetTintMode;
    SoftwareRenderer::BackendFunctions.SetTintEnabled = SoftwareRenderer::SetTintEnabled;
    SoftwareRenderer::BackendFunctions.SetLineWidth = SoftwareRenderer::SetLineWidth;

    // Primitive drawing functions
    SoftwareRenderer::BackendFunctions.StrokeLine = SoftwareRenderer::StrokeLine;
    SoftwareRenderer::BackendFunctions.StrokeCircle = SoftwareRenderer::StrokeCircle;
    SoftwareRenderer::BackendFunctions.StrokeEllipse = SoftwareRenderer::StrokeEllipse;
    SoftwareRenderer::BackendFunctions.StrokeRectangle = SoftwareRenderer::StrokeRectangle;
    SoftwareRenderer::BackendFunctions.FillCircle = SoftwareRenderer::FillCircle;
    SoftwareRenderer::BackendFunctions.FillEllipse = SoftwareRenderer::FillEllipse;
    SoftwareRenderer::BackendFunctions.FillTriangle = SoftwareRenderer::FillTriangle;
    SoftwareRenderer::BackendFunctions.FillRectangle = SoftwareRenderer::FillRectangle;

    // Texture drawing functions
    SoftwareRenderer::BackendFunctions.DrawTexture = SoftwareRenderer::DrawTexture;
    SoftwareRenderer::BackendFunctions.DrawSprite = SoftwareRenderer::DrawSprite;
    SoftwareRenderer::BackendFunctions.DrawSpritePart = SoftwareRenderer::DrawSpritePart;

    // 3D drawing functions
    SoftwareRenderer::BackendFunctions.DrawPolygon3D = SoftwareRenderer::DrawPolygon3D;
    SoftwareRenderer::BackendFunctions.DrawSceneLayer3D = SoftwareRenderer::DrawSceneLayer3D;
    SoftwareRenderer::BackendFunctions.DrawModel = SoftwareRenderer::DrawModel;
    SoftwareRenderer::BackendFunctions.DrawModelSkinned = SoftwareRenderer::DrawModelSkinned;
    SoftwareRenderer::BackendFunctions.DrawVertexBuffer = SoftwareRenderer::DrawVertexBuffer;
    SoftwareRenderer::BackendFunctions.BindVertexBuffer = SoftwareRenderer::BindVertexBuffer;
    SoftwareRenderer::BackendFunctions.UnbindVertexBuffer = SoftwareRenderer::UnbindVertexBuffer;
    SoftwareRenderer::BackendFunctions.BindScene3D = SoftwareRenderer::BindScene3D;
    SoftwareRenderer::BackendFunctions.DrawScene3D = SoftwareRenderer::DrawScene3D;

    SoftwareRenderer::BackendFunctions.MakeFrameBufferID = SoftwareRenderer::MakeFrameBufferID;
}
PUBLIC STATIC void     SoftwareRenderer::Dispose() {

}

PUBLIC STATIC void     SoftwareRenderer::RenderStart() {
    for (int i = 0; i < MAX_PALETTE_COUNT; i++)
        PaletteColors[i][0] &= 0xFFFFFF;
}
PUBLIC STATIC void     SoftwareRenderer::RenderEnd() {

}

// Texture management functions
PUBLIC STATIC Texture* SoftwareRenderer::CreateTexture(Uint32 format, Uint32 access, Uint32 width, Uint32 height) {
	Texture* texture = NULL; // Texture::New(format, access, width, height);

    return texture;
}
PUBLIC STATIC int      SoftwareRenderer::LockTexture(Texture* texture, void** pixels, int* pitch) {
    return 0;
}
PUBLIC STATIC int      SoftwareRenderer::UpdateTexture(Texture* texture, SDL_Rect* src, void* pixels, int pitch) {
    return 0;
}
PUBLIC STATIC int      SoftwareRenderer::UpdateYUVTexture(Texture* texture, SDL_Rect* src, Uint8* pixelsY, int pitchY, Uint8* pixelsU, int pitchU, Uint8* pixelsV, int pitchV) {
    return 0;
}
PUBLIC STATIC void     SoftwareRenderer::UnlockTexture(Texture* texture) {

}
PUBLIC STATIC void     SoftwareRenderer::DisposeTexture(Texture* texture) {

}

// Viewport and view-related functions
PUBLIC STATIC void     SoftwareRenderer::SetRenderTarget(Texture* texture) {

}
PUBLIC STATIC void     SoftwareRenderer::UpdateWindowSize(int width, int height) {
    Graphics::Internal.UpdateWindowSize(width, height);
}
PUBLIC STATIC void     SoftwareRenderer::UpdateViewport() {
    Graphics::Internal.UpdateViewport();
}
PUBLIC STATIC void     SoftwareRenderer::UpdateClipRect() {
    Graphics::Internal.UpdateClipRect();
}
PUBLIC STATIC void     SoftwareRenderer::UpdateOrtho(float left, float top, float right, float bottom) {
    Graphics::Internal.UpdateOrtho(left, top, right, bottom);
}
PUBLIC STATIC void     SoftwareRenderer::UpdatePerspective(float fovy, float aspect, float nearv, float farv) {
    Graphics::Internal.UpdatePerspective(fovy, aspect, nearv, farv);
}
PUBLIC STATIC void     SoftwareRenderer::UpdateProjectionMatrix() {
    Graphics::Internal.UpdateProjectionMatrix();
}
PUBLIC STATIC void     SoftwareRenderer::MakePerspectiveMatrix(Matrix4x4* out, float fov, float near, float far, float aspect) {
    float f = 1.0f / tanf(fov / 2.0f);
    float diff = near - far;

    out->Values[0]  = f / aspect;
    out->Values[1]  = 0.0f;
    out->Values[2]  = 0.0f;
    out->Values[3]  = 0.0f;

    out->Values[4]  = 0.0f;
    out->Values[5]  = -f;
    out->Values[6]  = 0.0f;
    out->Values[7]  = 0.0f;

    out->Values[8]  = 0.0f;
    out->Values[9]  = 0.0f;
    out->Values[10] = -(far + near) / diff;
    out->Values[11] = 1.0f;

    out->Values[12] = 0.0f;
    out->Values[13] = 0.0f;
    out->Values[14] = -(2.0f * far * near) / diff;
    out->Values[15] = 0.0f;
}

// Shader-related functions
PUBLIC STATIC void     SoftwareRenderer::UseShader(void* shader) {
    if (!shader) {
        CurrentBlendState.FilterTable = nullptr;
        return;
    }

    ObjArray* array = (ObjArray*)shader;

    if (Graphics::PreferredPixelFormat == SDL_PIXELFORMAT_ARGB8888) {
        for (Uint32 i = 0, iSz = (Uint32)array->Values->size(); i < 0x8000 && i < iSz; i++) {
            FilterCurrent[i] = AS_INTEGER((*array->Values)[i]) | 0xFF000000U;
        }
    }
    else {
        Uint8 px[4];
        Uint32 newI;
        for (Uint32 i = 0, iSz = (Uint32)array->Values->size(); i < 0x8000 && i < iSz; i++) {
            *(Uint32*)&px[0] = AS_INTEGER((*array->Values)[i]);
            newI = (i & 0x1F) << 10 | (i & 0x3E0) | (i & 0x7C00) >> 10;
            FilterCurrent[newI] = px[0] << 16 | px[1] << 8 | px[2] | 0xFF000000U;
        }
    }
    CurrentBlendState.FilterTable = &FilterCurrent[0];
}
PUBLIC STATIC void     SoftwareRenderer::SetUniformF(int location, int count, float* values) {

}
PUBLIC STATIC void     SoftwareRenderer::SetUniformI(int location, int count, int* values) {

}
PUBLIC STATIC void     SoftwareRenderer::SetUniformTexture(Texture* texture, int uniform_index, int slot) {

}

PUBLIC STATIC void     SoftwareRenderer::SetFilter(int filter) {
    switch (filter) {
    case Filter_NONE:
        CurrentBlendState.FilterTable = nullptr;
        break;
    case Filter_BLACK_AND_WHITE:
        CurrentBlendState.FilterTable = &FilterBlackAndWhite[0];
        break;
    case Filter_INVERT:
        CurrentBlendState.FilterTable = &FilterInvert[0];
        break;
    }
}

// These guys
PUBLIC STATIC void     SoftwareRenderer::Clear() {
    Uint32* dstPx = (Uint32*)Graphics::CurrentRenderTarget->Pixels;
    Uint32  dstStride = Graphics::CurrentRenderTarget->Width;
    memset(dstPx, 0, dstStride * Graphics::CurrentRenderTarget->Height * 4);
}
PUBLIC STATIC void     SoftwareRenderer::Present() {

}

// Draw mode setting functions
#define GET_R(color) ((color >> 16) & 0xFF)
#define GET_G(color) ((color >> 8) & 0xFF)
#define GET_B(color) ((color) & 0xFF)
PRIVATE STATIC void     SoftwareRenderer::SetColor(Uint32 color) {
    ColRGB = color;
    ColR = GET_R(color);
    ColG = GET_G(color);
    ColB = GET_B(color);
    Graphics::ConvertFromARGBtoNative(&ColRGB, 1);
}
PUBLIC STATIC void     SoftwareRenderer::SetBlendColor(float r, float g, float b, float a) {
    ColR = (Uint8)(r * 0xFF);
    ColG = (Uint8)(g * 0xFF);
    ColB = (Uint8)(b * 0xFF);
    ColRGB = ColorUtils::ToRGB(ColR, ColG, ColB);
    Graphics::ConvertFromARGBtoNative(&ColRGB, 1);

    int opacity = (int)(a * 0xFF);
    CLAMP_VAL(opacity, 0x00, 0xFF);
    CurrentBlendState.Opacity = opacity;
}
PUBLIC STATIC void     SoftwareRenderer::SetBlendMode(int srcC, int dstC, int srcA, int dstA) {
    CurrentBlendState.Mode = Graphics::BlendMode;
}
PUBLIC STATIC void     SoftwareRenderer::SetTintColor(float r, float g, float b, float a) {
    int red = (int)(r * 0xFF);
    int green = (int)(g * 0xFF);
    int blue = (int)(b * 0xFF);
    int alpha = (int)(a * 0x100);

    CLAMP_VAL(red, 0x00, 0xFF);
    CLAMP_VAL(green, 0x00, 0xFF);
    CLAMP_VAL(blue, 0x00, 0xFF);
    CLAMP_VAL(alpha, 0x00, 0x100);

    CurrentBlendState.Tint.Color = red << 16 | green << 8 | blue;
    CurrentBlendState.Tint.Amount = alpha;

    Graphics::ConvertFromARGBtoNative(&CurrentBlendState.Tint.Color, 1);
}
PUBLIC STATIC void     SoftwareRenderer::SetTintMode(int mode) {
    CurrentBlendState.Tint.Mode = mode;
}
PUBLIC STATIC void     SoftwareRenderer::SetTintEnabled(bool enabled) {
    CurrentBlendState.Tint.Enabled = enabled;
}

PUBLIC STATIC void     SoftwareRenderer::Resize(int width, int height) {

}

PUBLIC STATIC void     SoftwareRenderer::SetClip(float x, float y, float width, float height) {

}
PUBLIC STATIC void     SoftwareRenderer::ClearClip() {

}

PUBLIC STATIC void     SoftwareRenderer::Save() {

}
PUBLIC STATIC void     SoftwareRenderer::Translate(float x, float y, float z) {

}
PUBLIC STATIC void     SoftwareRenderer::Rotate(float x, float y, float z) {

}
PUBLIC STATIC void     SoftwareRenderer::Scale(float x, float y, float z) {

}
PUBLIC STATIC void     SoftwareRenderer::Restore() {

}

PRIVATE STATIC Uint32  SoftwareRenderer::GetBlendColor() {
    return ColorUtils::ToRGB(ColR, ColG, ColB);
}

PUBLIC STATIC int      SoftwareRenderer::ConvertBlendMode(int blendMode) {
    switch (blendMode) {
        case BlendMode_NORMAL:
            return BlendFlag_TRANSPARENT;
        case BlendMode_ADD:
            return BlendFlag_ADDITIVE;
        case BlendMode_SUBTRACT:
            return BlendFlag_SUBTRACT;
        case BlendMode_MATCH_EQUAL:
            return BlendFlag_MATCH_EQUAL;
        case BlendMode_MATCH_NOT_EQUAL:
            return BlendFlag_MATCH_NOT_EQUAL;
    }
    return BlendFlag_OPAQUE;
}
PUBLIC STATIC BlendState SoftwareRenderer::GetBlendState() {
    return CurrentBlendState;
}
PUBLIC STATIC bool     SoftwareRenderer::AlterBlendState(BlendState& state) {
    int blendMode = ConvertBlendMode(state.Mode);
    int opacity = state.Opacity;

    // Not visible
    if (opacity == 0 && blendMode == BlendFlag_TRANSPARENT)
        return false;

    // Switch to proper blend flag depending on opacity
    if (opacity != 0 && opacity < 0xFF && blendMode == BlendFlag_OPAQUE)
        blendMode = BlendFlag_TRANSPARENT;
    else if (opacity == 0xFF && blendMode == BlendFlag_TRANSPARENT)
        blendMode = BlendFlag_OPAQUE;

    state.Mode = blendMode;

    // Apply tint/filter flags
    if (state.Tint.Enabled)
        state.Mode |= BlendFlag_TINT_BIT;
    if (state.FilterTable != nullptr)
        state.Mode |= BlendFlag_TINT_BIT | BlendFlag_FILTER_BIT;

    return true;
}

// Filterless versions
#define ISOLATE_R(color) (color & 0xFF0000)
#define ISOLATE_G(color) (color & 0x00FF00)
#define ISOLATE_B(color) (color & 0x0000FF)

PUBLIC STATIC void SoftwareRenderer::PixelNoFiltSetOpaque(Uint32* src, Uint32* dst, BlendState& state, int* multTableAt, int* multSubTableAt) {
    *dst = *src;
}
PUBLIC STATIC void SoftwareRenderer::PixelNoFiltSetTransparent(Uint32* src, Uint32* dst, BlendState& state, int* multTableAt, int* multSubTableAt) {
    int* multInvTableAt = &MultTableInv[state.Opacity << 8];
    *dst = 0xFF000000U
        | (multTableAt[GET_R(*src)] + multInvTableAt[GET_R(*dst)]) << 16
        | (multTableAt[GET_G(*src)] + multInvTableAt[GET_G(*dst)]) << 8
        | (multTableAt[GET_B(*src)] + multInvTableAt[GET_B(*dst)]);
}
PUBLIC STATIC void SoftwareRenderer::PixelNoFiltSetAdditive(Uint32* src, Uint32* dst, BlendState& state, int* multTableAt, int* multSubTableAt) {
    Uint32 R = (multTableAt[GET_R(*src)] << 16) + ISOLATE_R(*dst);
    Uint32 G = (multTableAt[GET_G(*src)] << 8) + ISOLATE_G(*dst);
    Uint32 B = (multTableAt[GET_B(*src)]) + ISOLATE_B(*dst);
    if (R > 0xFF0000) R = 0xFF0000;
    if (G > 0x00FF00) G = 0x00FF00;
    if (B > 0x0000FF) B = 0x0000FF;
    *dst = 0xFF000000U | R | G | B;
}
PUBLIC STATIC void SoftwareRenderer::PixelNoFiltSetSubtract(Uint32* src, Uint32* dst, BlendState& state, int* multTableAt, int* multSubTableAt) {
    Sint32 R = (multSubTableAt[GET_R(*src)] << 16) + ISOLATE_R(*dst);
    Sint32 G = (multSubTableAt[GET_G(*src)] << 8) + ISOLATE_G(*dst);
    Sint32 B = (multSubTableAt[GET_B(*src)]) + ISOLATE_B(*dst);
    if (R < 0) R = 0;
    if (G < 0) G = 0;
    if (B < 0) B = 0;
    *dst = 0xFF000000U | R | G | B;
}
PUBLIC STATIC void SoftwareRenderer::PixelNoFiltSetMatchEqual(Uint32* src, Uint32* dst, BlendState& state, int* multTableAt, int* multSubTableAt) {
    // if (*dst == SoftwareRenderer::CompareColor)
        // *dst = *src;
    if ((*dst & 0xFCFCFC) == (SoftwareRenderer::CompareColor & 0xFCFCFC))
        *dst = *src;
}
PUBLIC STATIC void SoftwareRenderer::PixelNoFiltSetMatchNotEqual(Uint32* src, Uint32* dst, BlendState& state, int* multTableAt, int* multSubTableAt) {
    // if (*dst != SoftwareRenderer::CompareColor)
        // *dst = *src;
    if ((*dst & 0xFCFCFC) != (SoftwareRenderer::CompareColor & 0xFCFCFC))
        *dst = *src;
}

static void (*PixelNoFiltFunctions[])(Uint32*, Uint32*, BlendState&, int*, int*) = {
    SoftwareRenderer::PixelNoFiltSetOpaque,
    SoftwareRenderer::PixelNoFiltSetTransparent,
    SoftwareRenderer::PixelNoFiltSetAdditive,
    SoftwareRenderer::PixelNoFiltSetSubtract,
    SoftwareRenderer::PixelNoFiltSetMatchEqual,
    SoftwareRenderer::PixelNoFiltSetMatchNotEqual
};

// Tinted versions
PUBLIC STATIC void SoftwareRenderer::PixelTintSetOpaque(Uint32* src, Uint32* dst, BlendState& state, int* multTableAt, int* multSubTableAt) {
    *dst = TintFunction(src, dst, state.Tint.Color, state.Tint.Amount);
}
PUBLIC STATIC void SoftwareRenderer::PixelTintSetTransparent(Uint32* src, Uint32* dst, BlendState& state, int* multTableAt, int* multSubTableAt) {
    Uint32 col = TintFunction(src, dst, state.Tint.Color, state.Tint.Amount);
    PixelNoFiltSetTransparent(&col, dst, state, multTableAt, multSubTableAt);
}
PUBLIC STATIC void SoftwareRenderer::PixelTintSetAdditive(Uint32* src, Uint32* dst, BlendState& state, int* multTableAt, int* multSubTableAt) {
    Uint32 col = TintFunction(src, dst, state.Tint.Color, state.Tint.Amount);
    PixelNoFiltSetAdditive(&col, dst, state, multTableAt, multSubTableAt);
}
PUBLIC STATIC void SoftwareRenderer::PixelTintSetSubtract(Uint32* src, Uint32* dst, BlendState& state, int* multTableAt, int* multSubTableAt) {
    Uint32 col = TintFunction(src, dst, state.Tint.Color, state.Tint.Amount);
    PixelNoFiltSetSubtract(&col, dst, state, multTableAt, multSubTableAt);
}
PUBLIC STATIC void SoftwareRenderer::PixelTintSetMatchEqual(Uint32* src, Uint32* dst, BlendState& state, int* multTableAt, int* multSubTableAt) {
    if ((*dst & 0xFCFCFC) == (SoftwareRenderer::CompareColor & 0xFCFCFC))
        *dst = TintFunction(src, dst, state.Tint.Color, state.Tint.Amount);
}
PUBLIC STATIC void SoftwareRenderer::PixelTintSetMatchNotEqual(Uint32* src, Uint32* dst, BlendState& state, int* multTableAt, int* multSubTableAt) {
    if ((*dst & 0xFCFCFC) != (SoftwareRenderer::CompareColor & 0xFCFCFC))
        *dst = TintFunction(src, dst, state.Tint.Color, state.Tint.Amount);
}

static void (*PixelTintFunctions[])(Uint32*, Uint32*, BlendState&, int*, int*) = {
    SoftwareRenderer::PixelTintSetOpaque,
    SoftwareRenderer::PixelTintSetTransparent,
    SoftwareRenderer::PixelTintSetAdditive,
    SoftwareRenderer::PixelTintSetSubtract,
    SoftwareRenderer::PixelTintSetMatchEqual,
    SoftwareRenderer::PixelTintSetMatchNotEqual
};

#define GET_FILTER_COLOR(col) ((col & 0xF80000) >> 9 | (col & 0xF800) >> 6 | (col & 0xF8) >> 3)

static Uint32 TintNormalSource(Uint32* src, Uint32* dst, Uint32 tintColor, Uint32 tintAmount) {
    return ColorUtils::Tint(*src, tintColor, tintAmount);
}
static Uint32 TintNormalDest(Uint32* src, Uint32* dst, Uint32 tintColor, Uint32 tintAmount) {
    return ColorUtils::Tint(*dst, tintColor, tintAmount);
}
static Uint32 TintBlendSource(Uint32* src, Uint32* dst, Uint32 tintColor, Uint32 tintAmount) {
    return ColorUtils::Blend(*src, tintColor, tintAmount);
}
static Uint32 TintBlendDest(Uint32* src, Uint32* dst, Uint32 tintColor, Uint32 tintAmount) {
    return ColorUtils::Blend(*dst, tintColor, tintAmount);
}
static Uint32 TintFilterSource(Uint32* src, Uint32* dst, Uint32 tintColor, Uint32 tintAmount) {
    return CurrentBlendState.FilterTable[GET_FILTER_COLOR(*src)];
}
static Uint32 TintFilterDest(Uint32* src, Uint32* dst, Uint32 tintColor, Uint32 tintAmount) {
    return CurrentBlendState.FilterTable[GET_FILTER_COLOR(*dst)];
}

PUBLIC STATIC void     SoftwareRenderer::SetTintFunction(int blendFlags) {
    Uint32 (*tintFunctions[])(Uint32*, Uint32*, Uint32, Uint32) = {
        TintNormalSource,
        TintNormalDest,
        TintBlendSource,
        TintBlendDest
    };

    Uint32 (*filterFunctions[])(Uint32*, Uint32*, Uint32, Uint32) = {
        TintFilterSource,
        TintFilterDest
    };

    if (blendFlags & BlendFlag_FILTER_BIT)
        TintFunction = filterFunctions[CurrentBlendState.Tint.Mode & 1];
    else if (blendFlags & BlendFlag_TINT_BIT)
        TintFunction = tintFunctions[CurrentBlendState.Tint.Mode];
}

// Cases for PixelNoFiltSet
#define PIXEL_NO_FILT_CASES(drawMacro) \
    case BlendFlag_OPAQUE: \
        drawMacro(PixelNoFiltSetOpaque); \
        break; \
    case BlendFlag_TRANSPARENT: \
        drawMacro(PixelNoFiltSetTransparent); \
        break; \
    case BlendFlag_ADDITIVE: \
        drawMacro(PixelNoFiltSetAdditive); \
        break; \
    case BlendFlag_SUBTRACT: \
        drawMacro(PixelNoFiltSetSubtract); \
        break; \
    case BlendFlag_MATCH_EQUAL: \
        drawMacro(PixelNoFiltSetMatchEqual); \
        break; \
    case BlendFlag_MATCH_NOT_EQUAL: \
        drawMacro(PixelNoFiltSetMatchNotEqual); \
        break \

// Cases for PixelNoFiltSet (without BlendFlag_OPAQUE)
#define PIXEL_NO_FILT_CASES_NO_OPAQUE(drawMacro) \
    case BlendFlag_TRANSPARENT: \
        drawMacro(SoftwareRenderer::PixelNoFiltSetTransparent); \
        break; \
    case BlendFlag_ADDITIVE: \
        drawMacro(SoftwareRenderer::PixelNoFiltSetAdditive); \
        break; \
    case BlendFlag_SUBTRACT: \
        drawMacro(SoftwareRenderer::PixelNoFiltSetSubtract); \
        break; \
    case BlendFlag_MATCH_EQUAL: \
        drawMacro(SoftwareRenderer::PixelNoFiltSetMatchEqual); \
        break; \
    case BlendFlag_MATCH_NOT_EQUAL: \
        drawMacro(SoftwareRenderer::PixelNoFiltSetMatchNotEqual); \
        break \

// Cases for PixelTintSet
#define PIXEL_TINT_CASES(drawMacro) \
    case BlendFlag_OPAQUE | BlendFlag_TINT_BIT: \
        drawMacro(SoftwareRenderer::PixelTintSetOpaque); \
        break; \
    case BlendFlag_TRANSPARENT | BlendFlag_TINT_BIT: \
        drawMacro(SoftwareRenderer::PixelTintSetTransparent); \
        break; \
    case BlendFlag_ADDITIVE | BlendFlag_TINT_BIT: \
        drawMacro(SoftwareRenderer::PixelTintSetAdditive); \
        break; \
    case BlendFlag_SUBTRACT | BlendFlag_TINT_BIT: \
        drawMacro(SoftwareRenderer::PixelTintSetSubtract); \
        break; \
    case BlendFlag_MATCH_EQUAL | BlendFlag_TINT_BIT: \
        drawMacro(SoftwareRenderer::PixelTintSetMatchEqual); \
        break; \
    case BlendFlag_MATCH_NOT_EQUAL | BlendFlag_TINT_BIT: \
        drawMacro(SoftwareRenderer::PixelTintSetMatchNotEqual); \
        break \

// Cases for PixelNoFiltSet (for sprite images)
#define SPRITE_PIXEL_NO_FILT_CASES(drawMacro, placePixelMacro) \
    case BlendFlag_OPAQUE: \
        drawMacro(SoftwareRenderer::PixelNoFiltSetOpaque, placePixelMacro); \
        break; \
    case BlendFlag_TRANSPARENT: \
        drawMacro(SoftwareRenderer::PixelNoFiltSetTransparent, placePixelMacro); \
        break; \
    case BlendFlag_ADDITIVE: \
        drawMacro(SoftwareRenderer::PixelNoFiltSetAdditive, placePixelMacro); \
        break; \
    case BlendFlag_SUBTRACT: \
        drawMacro(SoftwareRenderer::PixelNoFiltSetSubtract, placePixelMacro); \
        break; \
    case BlendFlag_MATCH_EQUAL: \
        drawMacro(SoftwareRenderer::PixelNoFiltSetMatchEqual, placePixelMacro); \
        break; \
    case BlendFlag_MATCH_NOT_EQUAL: \
        drawMacro(SoftwareRenderer::PixelNoFiltSetMatchNotEqual, placePixelMacro); \
        break \

// Cases for PixelTintSet (for sprite images)
#define SPRITE_PIXEL_TINT_CASES(drawMacro, placePixelMacro) \
    case BlendFlag_OPAQUE | BlendFlag_TINT_BIT: \
        drawMacro(SoftwareRenderer::PixelTintSetOpaque, placePixelMacro); \
        break; \
    case BlendFlag_TRANSPARENT | BlendFlag_TINT_BIT: \
        drawMacro(SoftwareRenderer::PixelTintSetTransparent, placePixelMacro); \
        break; \
    case BlendFlag_ADDITIVE | BlendFlag_TINT_BIT: \
        drawMacro(SoftwareRenderer::PixelTintSetAdditive, placePixelMacro); \
        break; \
    case BlendFlag_SUBTRACT | BlendFlag_TINT_BIT: \
        drawMacro(SoftwareRenderer::PixelTintSetSubtract, placePixelMacro); \
        break; \
    case BlendFlag_MATCH_EQUAL | BlendFlag_TINT_BIT: \
        drawMacro(SoftwareRenderer::PixelTintSetMatchEqual, placePixelMacro); \
        break; \
    case BlendFlag_MATCH_NOT_EQUAL | BlendFlag_TINT_BIT: \
        drawMacro(SoftwareRenderer::PixelTintSetMatchNotEqual, placePixelMacro); \
        break \

// TODO: Material support
static int CalcVertexColor(Scene3D* scene, VertexAttribute *vertex, int normalY) {
    int col_r = GET_R(vertex->Color);
    int col_g = GET_G(vertex->Color);
    int col_b = GET_B(vertex->Color);
    int specularR = 0, specularG = 0, specularB = 0;

    Uint32 lightingAmbientR = (Uint32)(scene->Lighting.Ambient.R * 0x100);
    Uint32 lightingAmbientG = (Uint32)(scene->Lighting.Ambient.G * 0x100);
    Uint32 lightingAmbientB = (Uint32)(scene->Lighting.Ambient.B * 0x100);

    Uint32 lightingDiffuseR = Math::CeilPOT((int)(scene->Lighting.Diffuse.R * 0x100));
    Uint32 lightingDiffuseG = Math::CeilPOT((int)(scene->Lighting.Diffuse.G * 0x100));
    Uint32 lightingDiffuseB = Math::CeilPOT((int)(scene->Lighting.Diffuse.B * 0x100));

    Uint32 lightingSpecularR = Math::CeilPOT((int)(scene->Lighting.Specular.R * 0x100));
    Uint32 lightingSpecularG = Math::CeilPOT((int)(scene->Lighting.Specular.G * 0x100));
    Uint32 lightingSpecularB = Math::CeilPOT((int)(scene->Lighting.Specular.B * 0x100));

#define SHIFT_COL(color) { \
    int v = 0; \
    while (color) { color >>= 1; v++; } \
    color = --v; \
}

    SHIFT_COL(lightingDiffuseR);
    SHIFT_COL(lightingDiffuseG);
    SHIFT_COL(lightingDiffuseB);

    SHIFT_COL(lightingSpecularR);
    SHIFT_COL(lightingSpecularG);
    SHIFT_COL(lightingSpecularB);

#undef SHIFT_COL

    int ambientNormalY = normalY >> 10;
    int reweightedNormal = (normalY >> 2) * (abs(normalY) >> 2);

    // r
    col_r = (col_r * (ambientNormalY + lightingAmbientR)) >> lightingDiffuseR;
    specularR = reweightedNormal >> 6 >> lightingSpecularR;
    CLAMP_VAL(specularR, 0x00, 0xFF);
    specularR += col_r;
    CLAMP_VAL(specularR, 0x00, 0xFF);
    col_r = specularR;

    // g
    col_g = (col_g * (ambientNormalY + lightingAmbientG)) >> lightingDiffuseG;
    specularG = reweightedNormal >> 6 >> lightingSpecularG;
    CLAMP_VAL(specularG, 0x00, 0xFF);
    specularG += col_g;
    CLAMP_VAL(specularG, 0x00, 0xFF);
    col_g = specularG;

    // b
    col_b = (col_b * (ambientNormalY + lightingAmbientB)) >> lightingDiffuseB;
    specularB = reweightedNormal >> 6 >> lightingSpecularB;
    CLAMP_VAL(specularB, 0x00, 0xFF);
    specularB += col_b;
    CLAMP_VAL(specularB, 0x00, 0xFF);
    col_b = specularB;

    return col_r << 16 | col_g << 8 | col_b;
}

// Drawing 3D
PUBLIC STATIC void     SoftwareRenderer::BindVertexBuffer(Uint32 vertexBufferIndex) {

}
PUBLIC STATIC void     SoftwareRenderer::UnbindVertexBuffer() {

}
PUBLIC STATIC void     SoftwareRenderer::BindScene3D(Uint32 sceneIndex) {
    if (sceneIndex < 0 || sceneIndex >= MAX_3D_SCENES)
        return;

    Scene3D* scene = &Graphics::Scene3Ds[sceneIndex];
    if (scene->ClipPolygons) {
        polygonRenderer.BuildFrustumPlanes(scene->NearClippingPlane, scene->FarClippingPlane);
        polygonRenderer.ClipPolygonsByFrustum = true;
    }
    else
        polygonRenderer.ClipPolygonsByFrustum = false;
}
PUBLIC STATIC void     SoftwareRenderer::DrawScene3D(Uint32 sceneIndex, Uint32 drawMode) {
    if (sceneIndex < 0 || sceneIndex >= MAX_3D_SCENES)
        return;

    Scene3D* scene = &Graphics::Scene3Ds[sceneIndex];
    if (!scene->Initialized)
        return;

    View* currentView = &Scene::Views[Scene::ViewCurrent];
    int cx = (int)std::floor(currentView->X);
    int cy = (int)std::floor(currentView->Y);

    Matrix4x4* out = Graphics::ModelViewMatrix;
    int x = out->Values[12];
    int y = out->Values[13];
    x -= cx;
    y -= cy;

    bool doDepthTest = drawMode & DrawMode_DEPTH_TEST;
    bool usePerspective = !(drawMode & DrawMode_ORTHOGRAPHIC);

    PolygonRasterizer::SetDepthTest(doDepthTest);

    BlendState blendState;

#define SET_BLENDFLAG_AND_OPACITY(face) \
    if (!Graphics::TextureBlend) { \
        blendState.Mode = BlendFlag_OPAQUE; \
        blendState.Opacity = 0xFF; \
    } else { \
        blendState = face->Blend; \
        AlterBlendState(blendState); \
        if (!Graphics::UseTinting) \
            blendState.Tint.Enabled = false; \
    } \
    if ((blendState.Mode & BlendFlag_MODE_MASK) != BlendFlag_OPAQUE) \
        useDepthBuffer = false; \
    else \
        useDepthBuffer = doDepthTest; \
    PolygonRasterizer::SetUseDepthBuffer(useDepthBuffer)

    VertexBuffer* vertexBuffer = scene->Buffer;
    VertexAttribute* vertexAttribsPtr = vertexBuffer->Vertices; // R
    FaceInfo* faceInfoPtr = vertexBuffer->FaceInfoBuffer; // RW

    bool sortFaces = !doDepthTest && vertexBuffer->FaceCount > 1;
    if (Graphics::TextureBlend)
        sortFaces = true;

    // Get the face depth and vertices' start index
    Uint32 verticesStartIndex = 0;
    for (Uint32 f = 0; f < vertexBuffer->FaceCount; f++) {
        Uint32 vertexCount = faceInfoPtr->NumVertices;

        // Average the Z coordinates of the face
        if (sortFaces) {
            Sint64 depth = vertexAttribsPtr[0].Position.Z;
            for (Uint32 i = 1; i < vertexCount; i++)
                depth += vertexAttribsPtr[i].Position.Z;

            faceInfoPtr->Depth = depth / vertexCount;
            vertexAttribsPtr += vertexCount;
        }

        faceInfoPtr->DrawMode |= drawMode;
        faceInfoPtr->VerticesStartIndex = verticesStartIndex;
        verticesStartIndex += vertexCount;

        faceInfoPtr++;
    }

    // Sort face infos by depth
    if (sortFaces)
        qsort(vertexBuffer->FaceInfoBuffer, vertexBuffer->FaceCount, sizeof(FaceInfo), PolygonRenderer::FaceSortFunction);

    // sas
    for (Uint32 f = 0; f < vertexBuffer->FaceCount; f++) {
        Vector3 polygonVertex[MAX_POLYGON_VERTICES];
        Vector2 polygonUV[MAX_POLYGON_VERTICES];
        Uint32  polygonVertexIndex = 0;
        Uint32  numOutside = 0;

        faceInfoPtr = &vertexBuffer->FaceInfoBuffer[f];

        bool doAffineMapping = faceInfoPtr->DrawMode & DrawMode_AFFINE;
        bool useDepthBuffer;

        VertexAttribute *vertex, *vertexFirst;
        Uint32 vertexCount, vertexCountPerFace, vertexCountPerFaceMinus1;
        Texture *texturePtr;

        int widthSubpx = (int)(currentView->Width) << 16;
        int heightSubpx = (int)(currentView->Height) << 16;
        int widthHalfSubpx = widthSubpx >> 1;
        int heightHalfSubpx = heightSubpx >> 1;

#define PROJECT_X(pointX) ((pointX * currentView->Width * 0x10000) / vertexZ) - (cx << 16) + widthHalfSubpx
#define PROJECT_Y(pointY) ((pointY * currentView->Height * 0x10000) / vertexZ) - (cy << 16) + heightHalfSubpx

#define ORTHO_X(pointX) pointX - (cx << 16) + widthHalfSubpx
#define ORTHO_Y(pointY) pointY - (cy << 16) + heightHalfSubpx

        if (faceInfoPtr->DrawMode & DrawMode_FOG) {
            PolygonRasterizer::SetUseFog(true);
            PolygonRasterizer::SetFogColor(scene->Fog.Color.R, scene->Fog.Color.G, scene->Fog.Color.B);
            PolygonRasterizer::SetFogDensity(scene->Fog.Density);
        }
        else
            PolygonRasterizer::SetUseFog(false);

        switch (faceInfoPtr->DrawMode & DrawMode_FillTypeMask) {
            // Lines, Solid Colored
            case DrawMode_LINES:
                vertexCountPerFaceMinus1 = faceInfoPtr->NumVertices - 1;
                vertexFirst = &vertexBuffer->Vertices[faceInfoPtr->VerticesStartIndex];
                vertex = vertexFirst;

                SET_BLENDFLAG_AND_OPACITY(faceInfoPtr);
                CurrentBlendState = blendState;

                if (usePerspective) {
                    #define LINE_X(pos) ((float)PROJECT_X(pos)) / 0x10000
                    #define LINE_Y(pos) ((float)PROJECT_Y(pos)) / 0x10000
                    while (vertexCountPerFaceMinus1--) {
                        int vertexZ = vertex->Position.Z;
                        if (vertexZ < 0x10000)
                            goto mrt_line_solid_NEXT_FACE;

                        SetColor(vertex->Color);
                        SoftwareRenderer::StrokeLine(LINE_X(vertex[0].Position.X), LINE_Y(vertex[0].Position.Y), LINE_X(vertex[1].Position.X), LINE_Y(vertex[1].Position.Y));
                        vertex++;
                    }
                    int vertexZ = vertex->Position.Z;
                    if (vertexZ < 0x10000)
                        goto mrt_line_solid_NEXT_FACE;
                    SetColor(vertex->Color);
                    SoftwareRenderer::StrokeLine(LINE_X(vertex->Position.X), LINE_Y(vertex->Position.Y), LINE_X(vertexFirst->Position.X), LINE_Y(vertexFirst->Position.Y));
                }
                else {
                    #define LINE_ORTHO_X(pos) ((float)ORTHO_X(pos)) / 0x10000
                    #define LINE_ORTHO_Y(pos) ((float)ORTHO_Y(pos)) / 0x10000
                    while (vertexCountPerFaceMinus1--) {
                        SetColor(vertex->Color);
                        SoftwareRenderer::StrokeLine(LINE_ORTHO_X(vertex[0].Position.X), LINE_ORTHO_Y(vertex[0].Position.Y), LINE_ORTHO_X(vertex[1].Position.X), LINE_ORTHO_Y(vertex[1].Position.Y));
                        vertex++;
                    }
                    SetColor(vertex->Color);
                    SoftwareRenderer::StrokeLine(LINE_ORTHO_X(vertex->Position.X), LINE_ORTHO_Y(vertex->Position.Y), LINE_ORTHO_X(vertexFirst->Position.X), LINE_ORTHO_Y(vertexFirst->Position.Y));
                }

                mrt_line_solid_NEXT_FACE:
                faceInfoPtr++;
                break;
            // Lines, Flat Shading
            case DrawMode_LINES | DrawMode_FLAT_LIGHTING:
            // Lines, Smooth Shading
            case DrawMode_LINES | DrawMode_SMOOTH_LIGHTING: {
                vertexCount = faceInfoPtr->NumVertices;
                vertexCountPerFaceMinus1 = vertexCount - 1;
                vertexFirst = &vertexBuffer->Vertices[faceInfoPtr->VerticesStartIndex];
                vertex = vertexFirst;

                int averageNormalY = vertex[0].Normal.Y;
                for (Uint32 i = 1; i < vertexCount; i++)
                    averageNormalY += vertex[i].Normal.Y;
                averageNormalY /= vertexCount;

                SetColor(CalcVertexColor(scene, vertex, averageNormalY >> 8));
                SET_BLENDFLAG_AND_OPACITY(faceInfoPtr);
                CurrentBlendState = blendState;

                if (usePerspective) {
                    while (vertexCountPerFaceMinus1--) {
                        int vertexZ = vertex->Position.Z;
                        if (vertexZ < 0x10000)
                            goto mrt_line_smooth_NEXT_FACE;

                        SoftwareRenderer::StrokeLine(LINE_X(vertex[0].Position.X), LINE_Y(vertex[0].Position.Y), LINE_X(vertex[1].Position.X), LINE_Y(vertex[1].Position.Y));
                        vertex++;
                    }
                    int vertexZ = vertex->Position.Z;
                    if (vertexZ < 0x10000)
                        goto mrt_line_smooth_NEXT_FACE;
                    SoftwareRenderer::StrokeLine(LINE_X(vertex->Position.X), LINE_Y(vertex->Position.Y), LINE_X(vertexFirst->Position.X), LINE_Y(vertexFirst->Position.Y));
                    #undef LINE_X
                    #undef LINE_Y
                }
                else {
                    while (vertexCountPerFaceMinus1--) {
                        SoftwareRenderer::StrokeLine(LINE_ORTHO_X(vertex[0].Position.X), LINE_ORTHO_Y(vertex[0].Position.Y), LINE_ORTHO_X(vertex[1].Position.X), LINE_ORTHO_Y(vertex[1].Position.Y));
                        vertex++;
                    }
                    SoftwareRenderer::StrokeLine(LINE_ORTHO_X(vertex->Position.X), LINE_ORTHO_Y(vertex->Position.Y), LINE_ORTHO_X(vertexFirst->Position.X), LINE_ORTHO_Y(vertexFirst->Position.Y));
                    #undef LINE_ORTHO_X
                    #undef LINE_ORTHO_Y
                }

                mrt_line_smooth_NEXT_FACE:
                break;
            }
            // Polygons, Solid Colored
            case DrawMode_POLYGONS:
                vertexCount = vertexCountPerFace = faceInfoPtr->NumVertices;
                vertexFirst = &vertexBuffer->Vertices[faceInfoPtr->VerticesStartIndex];
                vertex = vertexFirst;

                SET_BLENDFLAG_AND_OPACITY(faceInfoPtr);

                while (vertexCountPerFace--) {
                    int vertexZ = vertex->Position.Z;
                    if (usePerspective) {
                        if (vertexZ < 0x10000)
                            goto mrt_poly_solid_NEXT_FACE;

                        polygonVertex[polygonVertexIndex].X = PROJECT_X(vertex->Position.X);
                        polygonVertex[polygonVertexIndex].Y = PROJECT_Y(vertex->Position.Y);
                    }
                    else {
                        polygonVertex[polygonVertexIndex].X = ORTHO_X(vertex->Position.X);
                        polygonVertex[polygonVertexIndex].Y = ORTHO_Y(vertex->Position.Y);
                    }

#define POINT_IS_OUTSIDE(i) \
    (polygonVertex[i].X < 0 || polygonVertex[i].Y < 0 || polygonVertex[i].X > widthSubpx || polygonVertex[i].Y > heightSubpx)

                    if (POINT_IS_OUTSIDE(polygonVertexIndex))
                        numOutside++;

                    polygonVertex[polygonVertexIndex].Z = vertexZ;
                    polygonUV[polygonVertexIndex].X = vertex->UV.X;
                    polygonUV[polygonVertexIndex].Y = vertex->UV.Y;
                    polygonVertexIndex++;
                    vertex++;
                }

                if (numOutside == vertexCount)
                    break;

                texturePtr = nullptr;
                if (faceInfoPtr->DrawMode & DrawMode_TEXTURED) {
                    if (faceInfoPtr->UseMaterial)
                        texturePtr = (Texture*)faceInfoPtr->MaterialInfo.Texture;
                }

                if (texturePtr) {
                    if (!doAffineMapping)
                        PolygonRasterizer::DrawPerspective(texturePtr, polygonVertex, polygonUV, vertexFirst->Color, vertexCount, blendState);
                    else
                        PolygonRasterizer::DrawAffine(texturePtr, polygonVertex, polygonUV, vertexFirst->Color, vertexCount, blendState);
                }
                else {
                    if (useDepthBuffer)
                        PolygonRasterizer::DrawDepth(polygonVertex, vertexFirst->Color, vertexCount, blendState);
                    else
                        PolygonRasterizer::DrawShaded(polygonVertex, vertexFirst->Color, vertexCount, blendState);
                }

                mrt_poly_solid_NEXT_FACE:
                break;
            // Polygons, Flat Shading
            case DrawMode_POLYGONS | DrawMode_FLAT_LIGHTING: {
                vertexCount = vertexCountPerFace = faceInfoPtr->NumVertices;
                vertexFirst = &vertexBuffer->Vertices[faceInfoPtr->VerticesStartIndex];
                vertex = vertexFirst;

                int averageNormalY = vertex[0].Normal.Y;
                for (Uint32 i = 1; i < vertexCount; i++)
                    averageNormalY += vertex[i].Normal.Y;
                averageNormalY /= vertexCount;

                int color = CalcVertexColor(scene, vertex, averageNormalY >> 8);
                SET_BLENDFLAG_AND_OPACITY(faceInfoPtr);

                while (vertexCountPerFace--) {
                    int vertexZ = vertex->Position.Z;
                    if (usePerspective) {
                        if (vertexZ < 0x10000)
                            goto mrt_poly_flat_NEXT_FACE;

                        polygonVertex[polygonVertexIndex].X = PROJECT_X(vertex->Position.X);
                        polygonVertex[polygonVertexIndex].Y = PROJECT_Y(vertex->Position.Y);
                    }
                    else {
                        polygonVertex[polygonVertexIndex].X = ORTHO_X(vertex->Position.X);
                        polygonVertex[polygonVertexIndex].Y = ORTHO_Y(vertex->Position.Y);
                    }

                    if (POINT_IS_OUTSIDE(polygonVertexIndex))
                        numOutside++;

                    polygonVertex[polygonVertexIndex].Z = vertexZ;
                    polygonUV[polygonVertexIndex].X = vertex->UV.X;
                    polygonUV[polygonVertexIndex].Y = vertex->UV.Y;
                    polygonVertexIndex++;
                    vertex++;
                }

                if (numOutside == vertexCount)
                    break;

                texturePtr = nullptr;
                if (faceInfoPtr->DrawMode & DrawMode_TEXTURED) {
                    if (faceInfoPtr->UseMaterial)
                        texturePtr = (Texture*)faceInfoPtr->MaterialInfo.Texture;
                }

                if (texturePtr) {
                    if (!doAffineMapping)
                        PolygonRasterizer::DrawPerspective(texturePtr, polygonVertex, polygonUV, color, vertexCount, blendState);
                    else
                        PolygonRasterizer::DrawAffine(texturePtr, polygonVertex, polygonUV, color, vertexCount, blendState);
                }
                else {
                    if (useDepthBuffer)
                        PolygonRasterizer::DrawDepth(polygonVertex, color, vertexCount, blendState);
                    else
                        PolygonRasterizer::DrawShaded(polygonVertex, color, vertexCount, blendState);
                }

                mrt_poly_flat_NEXT_FACE:
                break;
            }
            // Polygons, Smooth Shading
            case DrawMode_POLYGONS | DrawMode_SMOOTH_LIGHTING:
                vertexCount = vertexCountPerFace = faceInfoPtr->NumVertices;
                vertexFirst = &vertexBuffer->Vertices[faceInfoPtr->VerticesStartIndex];
                vertex = vertexFirst;

                SET_BLENDFLAG_AND_OPACITY(faceInfoPtr);

                Vector3 polygonVertex[MAX_POLYGON_VERTICES];
                Vector2 polygonUV[MAX_POLYGON_VERTICES];
                int     polygonVertColor[MAX_POLYGON_VERTICES];
                Uint32  polygonVertexIndex = 0;
                Uint32  numOutside = 0;
                while (vertexCountPerFace--) {
                    int vertexZ = vertex->Position.Z;
                    if (usePerspective) {
                        if (vertexZ < 0x10000)
                            goto mrt_poly_smooth_NEXT_FACE;

                        polygonVertex[polygonVertexIndex].X = PROJECT_X(vertex->Position.X);
                        polygonVertex[polygonVertexIndex].Y = PROJECT_Y(vertex->Position.Y);
                    }
                    else {
                        polygonVertex[polygonVertexIndex].X = ORTHO_X(vertex->Position.X);
                        polygonVertex[polygonVertexIndex].Y = ORTHO_Y(vertex->Position.Y);
                    }

                    if (POINT_IS_OUTSIDE(polygonVertexIndex))
                        numOutside++;

                    polygonVertex[polygonVertexIndex].Z = vertexZ;

                    polygonUV[polygonVertexIndex].X = vertex->UV.X;
                    polygonUV[polygonVertexIndex].Y = vertex->UV.Y;

                    polygonVertColor[polygonVertexIndex] = CalcVertexColor(scene, vertex, vertex->Normal.Y >> 8);
                    polygonVertexIndex++;
                    vertex++;
                }

                if (numOutside == vertexCount)
                    break;

#undef POINT_IS_OUTSIDE

                texturePtr = nullptr;
                if (faceInfoPtr->DrawMode & DrawMode_TEXTURED) {
                    if (faceInfoPtr->UseMaterial)
                        texturePtr = (Texture*)faceInfoPtr->MaterialInfo.Texture;
                }

                if (texturePtr) {
                    if (!doAffineMapping)
                        PolygonRasterizer::DrawBlendPerspective(texturePtr, polygonVertex, polygonUV, polygonVertColor, vertexCount, blendState);
                    else
                        PolygonRasterizer::DrawBlendAffine(texturePtr, polygonVertex, polygonUV, polygonVertColor, vertexCount, blendState);
                }
                else {
                    if (useDepthBuffer)
                        PolygonRasterizer::DrawBlendDepth(polygonVertex, polygonVertColor, vertexCount, blendState);
                    else
                        PolygonRasterizer::DrawBlendShaded(polygonVertex, polygonVertColor, vertexCount, blendState);
                }

                mrt_poly_smooth_NEXT_FACE:
                break;
            }
    }

#undef SET_BLENDFLAG_AND_OPACITY
#undef CHECK_TEXTURE

#undef PROJECT_X
#undef PROJECT_Y

#undef ORTHO_X
#undef ORTHO_Y
}

PRIVATE STATIC bool     SoftwareRenderer::SetupPolygonRenderer(Matrix4x4* modelMatrix, Matrix4x4* normalMatrix) {
    if (!polygonRenderer.SetBuffers())
        return false;

    polygonRenderer.DrawMode = polygonRenderer.ScenePtr ? polygonRenderer.ScenePtr->DrawMode : 0;
    polygonRenderer.DoProjection = true;
    polygonRenderer.DoClipping = true;
    polygonRenderer.ModelMatrix = modelMatrix;
    polygonRenderer.NormalMatrix = normalMatrix;
    polygonRenderer.CurrentColor = GetBlendColor();

    return true;
}

PUBLIC STATIC void     SoftwareRenderer::DrawPolygon3D(void* data, int vertexCount, int vertexFlag, Texture* texture, Matrix4x4* modelMatrix, Matrix4x4* normalMatrix) {
    if (SetupPolygonRenderer(modelMatrix, normalMatrix))
        polygonRenderer.DrawPolygon3D((VertexAttribute*)data, vertexCount, vertexFlag, texture);
}
PUBLIC STATIC void     SoftwareRenderer::DrawSceneLayer3D(void* layer, int sx, int sy, int sw, int sh, Matrix4x4* modelMatrix, Matrix4x4* normalMatrix) {
    if (SetupPolygonRenderer(modelMatrix, normalMatrix))
        polygonRenderer.DrawSceneLayer3D((SceneLayer*)layer, sx, sy, sw, sh);
}
PUBLIC STATIC void     SoftwareRenderer::DrawModel(void* model, Uint16 animation, Uint32 frame, Matrix4x4* modelMatrix, Matrix4x4* normalMatrix) {
    if (SetupPolygonRenderer(modelMatrix, normalMatrix))
        polygonRenderer.DrawModel((IModel*)model, animation, frame);
}
PUBLIC STATIC void     SoftwareRenderer::DrawModelSkinned(void* model, Uint16 armature, Matrix4x4* modelMatrix, Matrix4x4* normalMatrix) {
    if (SetupPolygonRenderer(modelMatrix, normalMatrix))
        polygonRenderer.DrawModelSkinned((IModel*)model, armature);
}
PUBLIC STATIC void     SoftwareRenderer::DrawVertexBuffer(Uint32 vertexBufferIndex, Matrix4x4* modelMatrix, Matrix4x4* normalMatrix) {
    if (Graphics::CurrentScene3D < 0)
        return;

    Scene3D* scene = &Graphics::Scene3Ds[Graphics::CurrentScene3D];
    if (!scene->Initialized)
        return;

    VertexBuffer* vertexBuffer = Graphics::VertexBuffers[vertexBufferIndex];
    if (!vertexBuffer)
        return;

    polygonRenderer.DoClipping = true;
    polygonRenderer.ScenePtr = scene;
    polygonRenderer.VertexBuf = vertexBuffer;
    polygonRenderer.ModelMatrix = modelMatrix;
    polygonRenderer.NormalMatrix = normalMatrix;
    polygonRenderer.CurrentColor = GetBlendColor();

    polygonRenderer.DrawVertexBuffer();
}

#undef SETUP_POLYGON_RENDERER

PUBLIC STATIC void     SoftwareRenderer::SetLineWidth(float n) {

}
PUBLIC STATIC void     SoftwareRenderer::StrokeLine(float x1, float y1, float x2, float y2) {
    int x = 0, y = 0;
    Uint32* dstPx = (Uint32*)Graphics::CurrentRenderTarget->Pixels;
    Uint32  dstStride = Graphics::CurrentRenderTarget->Width;

    View* currentView = &Scene::Views[Scene::ViewCurrent];
    int cx = (int)std::floor(currentView->X);
    int cy = (int)std::floor(currentView->Y);

    Matrix4x4* out = Graphics::ModelViewMatrix;
    x += out->Values[12];
    y += out->Values[13];
    x -= cx;
    y -= cy;

    int dst_x1 = x + x1;
    int dst_y1 = y + y1;
    int dst_x2 = x + x2;
    int dst_y2 = y + y2;

    int minX, maxX, minY, maxY;
    if (Graphics::CurrentClip.Enabled) {
        minX = Graphics::CurrentClip.X;
        minY = Graphics::CurrentClip.Y;
        maxX = Graphics::CurrentClip.X + Graphics::CurrentClip.Width;
        maxY = Graphics::CurrentClip.Y + Graphics::CurrentClip.Height;
    }
    else {
        minX = 0;
        minY = 0;
        maxX = (int)Graphics::CurrentRenderTarget->Width;
        maxY = (int)Graphics::CurrentRenderTarget->Height;
    }

    int dx = Math::Abs(dst_x2 - dst_x1), sx = dst_x1 < dst_x2 ? 1 : -1;
    int dy = Math::Abs(dst_y2 - dst_y1), sy = dst_y1 < dst_y2 ? 1 : -1;
    int err = (dx > dy ? dx : -dy) / 2, e2;

    BlendState blendState = GetBlendState();
    if (!AlterBlendState(blendState))
        return;

    Uint32 col = ColRGB;
    int blendFlag = blendState.Mode;
    int opacity = blendState.Opacity;

    #define DRAW_LINE(pixelFunction) while (true) { \
        if (dst_x1 >= minX && dst_y1 >= minY && dst_x1 < maxX && dst_y1 < maxY) \
            pixelFunction((Uint32*)&col, &dstPx[dst_x1 + dst_y1 * dstStride], blendState, multTableAt, multSubTableAt); \
        if (dst_x1 == dst_x2 && dst_y1 == dst_y2) break; \
        e2 = err; \
        if (e2 > -dx) { err -= dy; dst_x1 += sx; } \
        if (e2 <  dy) { err += dx; dst_y1 += sy; } \
    }

    if (blendFlag & (BlendFlag_TINT_BIT | BlendFlag_FILTER_BIT))
        SetTintFunction(blendFlag);

    int* multTableAt = &MultTable[opacity << 8];
    int* multSubTableAt = &MultSubTable[opacity << 8];
    switch (blendFlag & (BlendFlag_MODE_MASK | BlendFlag_TINT_BIT)) {
        PIXEL_NO_FILT_CASES(DRAW_LINE);
        PIXEL_TINT_CASES(DRAW_LINE);
    }

    #undef DRAW_LINE
}
PUBLIC STATIC void     SoftwareRenderer::StrokeCircle(float x, float y, float rad) {

}
PUBLIC STATIC void     SoftwareRenderer::StrokeEllipse(float x, float y, float w, float h) {

}
PUBLIC STATIC void     SoftwareRenderer::StrokeRectangle(float x, float y, float w, float h) {

}

PUBLIC STATIC void     SoftwareRenderer::FillCircle(float x, float y, float rad) {
    // just checks to see if the pixel is within a radius range, uses a bounding box constructed by the diameter

    Uint32* dstPx = (Uint32*)Graphics::CurrentRenderTarget->Pixels;
    Uint32  dstStride = Graphics::CurrentRenderTarget->Width;

    View* currentView = &Scene::Views[Scene::ViewCurrent];
    int cx = (int)std::floor(currentView->X);
    int cy = (int)std::floor(currentView->Y);

    Matrix4x4* out = Graphics::ModelViewMatrix;
    x += out->Values[12];
    y += out->Values[13];
    x -= cx;
    y -= cy;

    int dst_x1 = x - rad;
    int dst_y1 = y - rad;
    int dst_x2 = x + rad;
    int dst_y2 = y + rad;

    if (Graphics::CurrentClip.Enabled) {
        if (dst_x2 > Graphics::CurrentClip.X + Graphics::CurrentClip.Width)
            dst_x2 = Graphics::CurrentClip.X + Graphics::CurrentClip.Width;
        if (dst_y2 > Graphics::CurrentClip.Y + Graphics::CurrentClip.Height)
            dst_y2 = Graphics::CurrentClip.Y + Graphics::CurrentClip.Height;

        if (dst_x1 < Graphics::CurrentClip.X)
            dst_x1 = Graphics::CurrentClip.X;
        if (dst_y1 < Graphics::CurrentClip.Y)
            dst_y1 = Graphics::CurrentClip.Y;
    }
    else {
        if (dst_x2 > (int)Graphics::CurrentRenderTarget->Width)
            dst_x2 = (int)Graphics::CurrentRenderTarget->Width;
        if (dst_y2 > (int)Graphics::CurrentRenderTarget->Height)
            dst_y2 = (int)Graphics::CurrentRenderTarget->Height;

        if (dst_x1 < 0)
            dst_x1 = 0;
        if (dst_y1 < 0)
            dst_y1 = 0;
    }

    if (dst_x1 >= dst_x2)
        return;
    if (dst_y1 >= dst_y2)
        return;

    BlendState blendState = GetBlendState();
    if (!AlterBlendState(blendState))
        return;

    int blendFlag = blendState.Mode;
    int opacity = blendState.Opacity;

    int scanLineCount = dst_y2 - dst_y1 + 1;
    Contour* contourPtr = &ContourBuffer[dst_y1];
    while (scanLineCount--) {
        contourPtr->MinX = 0x7FFFFFFF;
        contourPtr->MaxX = -1;
        contourPtr++;
    }

    #define SEEK_MIN(our_x, our_y) if (our_y >= dst_y1 && our_y < dst_y2 && our_x < (cont = &ContourBuffer[our_y])->MinX) \
        cont->MinX = our_x < dst_x1 ? dst_x1 : our_x > (dst_x2 - 1) ? dst_x2 - 1 : our_x;
    #define SEEK_MAX(our_x, our_y) if (our_y >= dst_y1 && our_y < dst_y2 && our_x > (cont = &ContourBuffer[our_y])->MaxX) \
        cont->MaxX = our_x < dst_x1 ? dst_x1 : our_x > (dst_x2 - 1) ? dst_x2 - 1 : our_x;

    Contour* cont;
    int ccx = x, ccy = y;
    int bx = 0, by = rad;
    int bd = 3 - 2 * rad;
    while (bx <= by) {
        if (bd <= 0) {
            bd += 4 * bx + 6;
        }
        else {
            bd += 4 * (bx - by) + 10;
            by--;
        }
        bx++;
        SEEK_MAX(ccx + bx, ccy - by);
        SEEK_MIN(ccx - bx, ccy - by);
        SEEK_MAX(ccx + by, ccy - bx);
        SEEK_MIN(ccx - by, ccy - bx);
        ccy--;
        SEEK_MAX(ccx + bx, ccy + by);
        SEEK_MIN(ccx - bx, ccy + by);
        SEEK_MAX(ccx + by, ccy + bx);
        SEEK_MIN(ccx - by, ccy + bx);
        ccy++;
    }

    Uint32 col = ColRGB;

    #define DRAW_CIRCLE(pixelFunction) for (int dst_y = dst_y1; dst_y < dst_y2; dst_y++) { \
        Contour contour = ContourBuffer[dst_y]; \
        if (contour.MaxX < contour.MinX) { \
            dst_strideY += dstStride; \
            continue; \
        } \
        for (int dst_x = contour.MinX >= dst_x1 ? contour.MinX : dst_x1; dst_x < contour.MaxX && dst_x < dst_x2; dst_x++) { \
            pixelFunction((Uint32*)&col, &dstPx[dst_x + dst_strideY], blendState, multTableAt, multSubTableAt); \
        } \
        dst_strideY += dstStride; \
    }

    if (blendFlag & (BlendFlag_TINT_BIT | BlendFlag_FILTER_BIT))
        SetTintFunction(blendFlag);

    int* multTableAt = &MultTable[opacity << 8];
    int* multSubTableAt = &MultSubTable[opacity << 8];
    int dst_strideY = dst_y1 * dstStride;
    switch (blendFlag & (BlendFlag_MODE_MASK | BlendFlag_TINT_BIT)) {
        case BlendFlag_OPAQUE:
            for (int dst_y = dst_y1; dst_y < dst_y2; dst_y++) {
                Contour contour = ContourBuffer[dst_y];
                if (contour.MaxX < contour.MinX) {
                    dst_strideY += dstStride;
                    continue;
                }
                int dst_min_x = contour.MinX;
                if (dst_min_x < dst_x1)
                    dst_min_x = dst_x1;
                int dst_max_x = contour.MaxX;
                if (dst_max_x > dst_x2 - 1)
                    dst_max_x = dst_x2 - 1;

                Memory::Memset4(&dstPx[dst_min_x + dst_strideY], col, dst_max_x - dst_min_x);
                dst_strideY += dstStride;
            }

            break;
        PIXEL_NO_FILT_CASES_NO_OPAQUE(DRAW_CIRCLE);
        PIXEL_TINT_CASES(DRAW_CIRCLE);
    }

    #undef DRAW_CIRCLE
}
PUBLIC STATIC void     SoftwareRenderer::FillEllipse(float x, float y, float w, float h) {

}
PUBLIC STATIC void     SoftwareRenderer::FillRectangle(float x, float y, float w, float h) {
    Uint32* dstPx = (Uint32*)Graphics::CurrentRenderTarget->Pixels;
    Uint32  dstStride = Graphics::CurrentRenderTarget->Width;

    View* currentView = &Scene::Views[Scene::ViewCurrent];
    int cx = (int)std::floor(currentView->X);
    int cy = (int)std::floor(currentView->Y);

    Matrix4x4* out = Graphics::ModelViewMatrix;
    x += out->Values[12];
    y += out->Values[13];
    x -= cx;
    y -= cy;

    int dst_x1 = x;
    int dst_y1 = y;
    int dst_x2 = x + w;
    int dst_y2 = y + h;

    if (Graphics::CurrentClip.Enabled) {
        if (dst_x2 > Graphics::CurrentClip.X + Graphics::CurrentClip.Width)
            dst_x2 = Graphics::CurrentClip.X + Graphics::CurrentClip.Width;
        if (dst_y2 > Graphics::CurrentClip.Y + Graphics::CurrentClip.Height)
            dst_y2 = Graphics::CurrentClip.Y + Graphics::CurrentClip.Height;

        if (dst_x1 < Graphics::CurrentClip.X)
            dst_x1 = Graphics::CurrentClip.X;
        if (dst_y1 < Graphics::CurrentClip.Y)
            dst_y1 = Graphics::CurrentClip.Y;
    }
    else {
        if (dst_x2 > (int)Graphics::CurrentRenderTarget->Width)
            dst_x2 = (int)Graphics::CurrentRenderTarget->Width;
        if (dst_y2 > (int)Graphics::CurrentRenderTarget->Height)
            dst_y2 = (int)Graphics::CurrentRenderTarget->Height;

        if (dst_x1 < 0)
            dst_x1 = 0;
        if (dst_y1 < 0)
            dst_y1 = 0;
    }

    if (dst_x1 >= dst_x2)
        return;
    if (dst_y1 >= dst_y2)
        return;

    BlendState blendState = GetBlendState();
    if (!AlterBlendState(blendState))
        return;

    Uint32 col = ColRGB;
    int blendFlag = blendState.Mode;
    int opacity = blendState.Opacity;

    #define DRAW_RECT(pixelFunction) for (int dst_y = dst_y1; dst_y < dst_y2; dst_y++) { \
        for (int dst_x = dst_x1; dst_x < dst_x2; dst_x++) { \
            pixelFunction((Uint32*)&col, &dstPx[dst_x + dst_strideY], blendState, multTableAt, multSubTableAt); \
        } \
        dst_strideY += dstStride; \
    }

    if (blendFlag & (BlendFlag_TINT_BIT | BlendFlag_FILTER_BIT))
        SetTintFunction(blendFlag);

    int* multTableAt = &MultTable[opacity << 8];
    int* multSubTableAt = &MultSubTable[opacity << 8];
    int dst_strideY = dst_y1 * dstStride;
    switch (blendFlag & (BlendFlag_MODE_MASK | BlendFlag_TINT_BIT)) {
        case BlendFlag_OPAQUE:
            for (int dst_y = dst_y1; dst_y < dst_y2; dst_y++) {
                Memory::Memset4(&dstPx[dst_x1 + dst_strideY], col, dst_x2 - dst_x1);
                dst_strideY += dstStride;
            }
            break;
        PIXEL_NO_FILT_CASES_NO_OPAQUE(DRAW_RECT);
        PIXEL_TINT_CASES(DRAW_RECT);
    }

    #undef DRAW_RECT
}
PUBLIC STATIC void     SoftwareRenderer::FillTriangle(float x1, float y1, float x2, float y2, float x3, float y3) {
    int x = 0, y = 0;
    View* currentView = &Scene::Views[Scene::ViewCurrent];
    int cx = (int)std::floor(currentView->X);
    int cy = (int)std::floor(currentView->Y);

    Matrix4x4* out = Graphics::ModelViewMatrix;
    x += out->Values[12];
    y += out->Values[13];
    x -= cx;
    y -= cy;

    Vector2 vectors[3];
    vectors[0].X = ((int)x1 + x) << 16; vectors[0].Y = ((int)y1 + y) << 16;
    vectors[1].X = ((int)x2 + x) << 16; vectors[1].Y = ((int)y2 + y) << 16;
    vectors[2].X = ((int)x3 + x) << 16; vectors[2].Y = ((int)y3 + y) << 16;
    PolygonRasterizer::DrawBasic(vectors, ColRGB, 3, GetBlendState());
}
PUBLIC STATIC void     SoftwareRenderer::FillTriangleBlend(float x1, float y1, float x2, float y2, float x3, float y3, int c1, int c2, int c3) {
    int x = 0, y = 0;
    View* currentView = &Scene::Views[Scene::ViewCurrent];
    int cx = (int)std::floor(currentView->X);
    int cy = (int)std::floor(currentView->Y);

    Matrix4x4* out = Graphics::ModelViewMatrix;
    x += out->Values[12];
    y += out->Values[13];
    x -= cx;
    y -= cy;

    int colors[3];
    Vector2 vectors[3];
    vectors[0].X = ((int)x1 + x) << 16; vectors[0].Y = ((int)y1 + y) << 16; colors[0] = ColorUtils::Multiply(c1, GetBlendColor());
    vectors[1].X = ((int)x2 + x) << 16; vectors[1].Y = ((int)y2 + y) << 16; colors[1] = ColorUtils::Multiply(c2, GetBlendColor());
    vectors[2].X = ((int)x3 + x) << 16; vectors[2].Y = ((int)y3 + y) << 16; colors[2] = ColorUtils::Multiply(c3, GetBlendColor());
    PolygonRasterizer::DrawBasicBlend(vectors, colors, 3, GetBlendState());
}
PUBLIC STATIC void     SoftwareRenderer::FillQuadBlend(float x1, float y1, float x2, float y2, float x3, float y3, float x4, float y4, int c1, int c2, int c3, int c4) {
    int x = 0, y = 0;
    View* currentView = &Scene::Views[Scene::ViewCurrent];
    int cx = (int)std::floor(currentView->X);
    int cy = (int)std::floor(currentView->Y);

    Matrix4x4* out = Graphics::ModelViewMatrix;
    x += out->Values[12];
    y += out->Values[13];
    x -= cx;
    y -= cy;

    int colors[4];
    Vector2 vectors[4];
    vectors[0].X = ((int)x1 + x) << 16; vectors[0].Y = ((int)y1 + y) << 16; colors[0] = ColorUtils::Multiply(c1, GetBlendColor());
    vectors[1].X = ((int)x2 + x) << 16; vectors[1].Y = ((int)y2 + y) << 16; colors[1] = ColorUtils::Multiply(c2, GetBlendColor());
    vectors[2].X = ((int)x3 + x) << 16; vectors[2].Y = ((int)y3 + y) << 16; colors[2] = ColorUtils::Multiply(c3, GetBlendColor());
    vectors[3].X = ((int)x4 + x) << 16; vectors[3].Y = ((int)y4 + y) << 16; colors[3] = ColorUtils::Multiply(c4, GetBlendColor());
    PolygonRasterizer::DrawBasicBlend(vectors, colors, 4, GetBlendState());
}

void DrawSpriteImage(Texture* texture, int x, int y, int w, int h, int sx, int sy, int flipFlag, BlendState blendState) {
    Uint32* srcPx = (Uint32*)texture->Pixels;
    Uint32  srcStride = texture->Width;
    Uint32* srcPxLine;

    Uint32* dstPx = (Uint32*)Graphics::CurrentRenderTarget->Pixels;
    Uint32  dstStride = Graphics::CurrentRenderTarget->Width;
    Uint32* dstPxLine;

    int src_x1 = sx;
    int src_y1 = sy;
    int src_x2 = sx + w - 1;
    int src_y2 = sy + h - 1;

    int dst_x1 = x;
    int dst_y1 = y;
    int dst_x2 = x + w;
    int dst_y2 = y + h;

    if (!Graphics::TextureBlend) {
        blendState.Mode = BlendMode_NORMAL;
        blendState.Opacity = 0xFF;
    }

    if (!SoftwareRenderer::AlterBlendState(blendState))
        return;

    int blendFlag = blendState.Mode;
    int opacity = blendState.Opacity;

    int clip_x1 = 0,
        clip_y1 = 0,
        clip_x2 = 0,
        clip_y2 = 0;

    if (Graphics::CurrentClip.Enabled) {
        clip_x1 = Graphics::CurrentClip.X;
        clip_y1 = Graphics::CurrentClip.Y;
        clip_x2 = Graphics::CurrentClip.X + Graphics::CurrentClip.Width;
        clip_y2 = Graphics::CurrentClip.Y + Graphics::CurrentClip.Height;

        if (dst_x2 > clip_x2)
            dst_x2 = clip_x2;
        if (dst_y2 > clip_y2)
            dst_y2 = clip_y2;

        if (dst_x1 < clip_x1) {
            src_x1 += clip_x1 - dst_x1;
            src_x2 -= clip_x1 - dst_x1;
            dst_x1 = clip_x1;
        }
        if (dst_y1 < clip_y1) {
            src_y1 += clip_y1 - dst_y1;
            src_y2 -= clip_y1 - dst_y1;
            dst_y1 = clip_y1;
        }
    }
    else {
        clip_x2 = (int)Graphics::CurrentRenderTarget->Width,
        clip_y2 = (int)Graphics::CurrentRenderTarget->Height;

        if (dst_x2 > clip_x2)
            dst_x2 = clip_x2;
        if (dst_y2 > clip_y2)
            dst_y2 = clip_y2;

        if (dst_x1 < 0) {
            src_x1 += -dst_x1;
            src_x2 -= -dst_x1;
            dst_x1 = 0;
        }
        if (dst_y1 < 0) {
            src_y1 += -dst_y1;
            src_y2 -= -dst_y1;
            dst_y1 = 0;
        }
    }

    if (dst_x1 >= dst_x2)
        return;
    if (dst_y1 >= dst_y2)
        return;

    #define DEFORM_X { \
        dst_x += *deformValues; \
        if (dst_x < clip_x1) { \
            dst_x -= *deformValues; \
            continue; \
        } \
        if (dst_x >= clip_x2) { \
            dst_x -= *deformValues; \
            continue; \
        } \
    }

    #define DRAW_PLACEPIXEL(pixelFunction) \
        if ((color = srcPxLine[src_x]) & 0xFF000000U) \
            pixelFunction(&color, &dstPxLine[dst_x], blendState, multTableAt, multSubTableAt);
    #define DRAW_PLACEPIXEL_PAL(pixelFunction) \
        if ((color = srcPxLine[src_x])) \
            pixelFunction(&index[color], &dstPxLine[dst_x], blendState, multTableAt, multSubTableAt);

    #define DRAW_NOFLIP(pixelFunction, placePixelMacro) \
    for (int dst_y = dst_y1; dst_y < dst_y2; dst_y++) { \
        srcPxLine = srcPx + src_strideY; \
        dstPxLine = dstPx + dst_strideY; \
        index = &SoftwareRenderer::PaletteColors[SoftwareRenderer::PaletteIndexLines[dst_y]][0]; \
        if (SoftwareRenderer::UseSpriteDeform) \
            for (int dst_x = dst_x1, src_x = src_x1; dst_x < dst_x2; dst_x++, src_x++) { \
                DEFORM_X; \
                placePixelMacro(pixelFunction) \
                dst_x -= *deformValues;\
            } \
        else \
            for (int dst_x = dst_x1, src_x = src_x1; dst_x < dst_x2; dst_x++, src_x++) { \
                placePixelMacro(pixelFunction) \
            } \
        \
        dst_strideY += dstStride; src_strideY += srcStride; deformValues++; \
    }
    #define DRAW_FLIPX(pixelFunction, placePixelMacro) for (int dst_y = dst_y1; dst_y < dst_y2; dst_y++) { \
        srcPxLine = srcPx + src_strideY; \
        dstPxLine = dstPx + dst_strideY; \
        index = &SoftwareRenderer::PaletteColors[SoftwareRenderer::PaletteIndexLines[dst_y]][0]; \
        if (SoftwareRenderer::UseSpriteDeform) \
            for (int dst_x = dst_x1, src_x = src_x2; dst_x < dst_x2; dst_x++, src_x--) { \
                DEFORM_X; \
                placePixelMacro(pixelFunction) \
                dst_x -= *deformValues;\
            } \
        else \
            for (int dst_x = dst_x1, src_x = src_x2; dst_x < dst_x2; dst_x++, src_x--) { \
                placePixelMacro(pixelFunction) \
            } \
        dst_strideY += dstStride; src_strideY += srcStride; \
        deformValues++; \
    }
    #define DRAW_FLIPY(pixelFunction, placePixelMacro) for (int dst_y = dst_y1; dst_y < dst_y2; dst_y++) { \
        srcPxLine = srcPx + src_strideY; \
        dstPxLine = dstPx + dst_strideY; \
        index = &SoftwareRenderer::PaletteColors[SoftwareRenderer::PaletteIndexLines[dst_y]][0]; \
        if (SoftwareRenderer::UseSpriteDeform) \
            for (int dst_x = dst_x1, src_x = src_x1; dst_x < dst_x2; dst_x++, src_x++) { \
                DEFORM_X; \
                placePixelMacro(pixelFunction) \
                dst_x -= *deformValues;\
            } \
        else \
            for (int dst_x = dst_x1, src_x = src_x1; dst_x < dst_x2; dst_x++, src_x++) { \
                placePixelMacro(pixelFunction) \
            } \
        dst_strideY += dstStride; src_strideY -= srcStride; \
        deformValues++; \
    }
    #define DRAW_FLIPXY(pixelFunction, placePixelMacro) for (int dst_y = dst_y1; dst_y < dst_y2; dst_y++) { \
        srcPxLine = srcPx + src_strideY; \
        dstPxLine = dstPx + dst_strideY; \
        index = &SoftwareRenderer::PaletteColors[SoftwareRenderer::PaletteIndexLines[dst_y]][0]; \
        if (SoftwareRenderer::UseSpriteDeform) \
            for (int dst_x = dst_x1, src_x = src_x2; dst_x < dst_x2; dst_x++, src_x--) { \
                DEFORM_X; \
                placePixelMacro(pixelFunction) \
                dst_x -= *deformValues;\
            } \
        else \
            for (int dst_x = dst_x1, src_x = src_x2; dst_x < dst_x2; dst_x++, src_x--) { \
                placePixelMacro(pixelFunction) \
            } \
        dst_strideY += dstStride; src_strideY -= srcStride; \
        deformValues++; \
    }

    #define BLENDFLAGS(flipMacro, placePixelMacro) \
        switch (blendFlag & (BlendFlag_MODE_MASK | BlendFlag_TINT_BIT)) { \
            SPRITE_PIXEL_NO_FILT_CASES(flipMacro, placePixelMacro); \
            SPRITE_PIXEL_TINT_CASES(flipMacro, placePixelMacro); \
        }

    if (blendFlag & (BlendFlag_TINT_BIT | BlendFlag_FILTER_BIT))
        SoftwareRenderer::SetTintFunction(blendFlag);

    Uint32 color;
    Uint32* index;
    int dst_strideY, src_strideY;
    int* multTableAt = &SoftwareRenderer::MultTable[opacity << 8];
    int* multSubTableAt = &SoftwareRenderer::MultSubTable[opacity << 8];
    Sint32* deformValues = &SoftwareRenderer::SpriteDeformBuffer[dst_y1];

    if (Graphics::UsePalettes && texture->Paletted) {
        switch (flipFlag) {
            case 0:
                dst_strideY = dst_y1 * dstStride;
                src_strideY = src_y1 * srcStride;
                BLENDFLAGS(DRAW_NOFLIP, DRAW_PLACEPIXEL_PAL);
                break;
            case 1:
                dst_strideY = dst_y1 * dstStride;
                src_strideY = src_y1 * srcStride;
                BLENDFLAGS(DRAW_FLIPX, DRAW_PLACEPIXEL_PAL);
                break;
            case 2:
                dst_strideY = dst_y1 * dstStride;
                src_strideY = src_y2 * srcStride;
                BLENDFLAGS(DRAW_FLIPY, DRAW_PLACEPIXEL_PAL);
                break;
            case 3:
                dst_strideY = dst_y1 * dstStride;
                src_strideY = src_y2 * srcStride;
                BLENDFLAGS(DRAW_FLIPXY, DRAW_PLACEPIXEL_PAL);
                break;
        }
    }
    else {
        switch (flipFlag) {
            case 0:
                dst_strideY = dst_y1 * dstStride;
                src_strideY = src_y1 * srcStride;
                BLENDFLAGS(DRAW_NOFLIP, DRAW_PLACEPIXEL);
                break;
            case 1:
                dst_strideY = dst_y1 * dstStride;
                src_strideY = src_y1 * srcStride;
                BLENDFLAGS(DRAW_FLIPX, DRAW_PLACEPIXEL);
                break;
            case 2:
                dst_strideY = dst_y1 * dstStride;
                src_strideY = src_y2 * srcStride;
                BLENDFLAGS(DRAW_FLIPY, DRAW_PLACEPIXEL);
                break;
            case 3:
                dst_strideY = dst_y1 * dstStride;
                src_strideY = src_y2 * srcStride;
                BLENDFLAGS(DRAW_FLIPXY, DRAW_PLACEPIXEL);
                break;
        }
    }

    #undef DRAW_PLACEPIXEL
    #undef DRAW_PLACEPIXEL_PAL
    #undef DRAW_NOFLIP
    #undef DRAW_FLIPX
    #undef DRAW_FLIPY
    #undef DRAW_FLIPXY
    #undef BLENDFLAGS
}
void DrawSpriteImageTransformed(Texture* texture, int x, int y, int offx, int offy, int w, int h, int sx, int sy, int sw, int sh, int flipFlag, int rotation, BlendState blendState) {
    Uint32* srcPx = (Uint32*)texture->Pixels;
    Uint32  srcStride = texture->Width;

    Uint32* dstPx = (Uint32*)Graphics::CurrentRenderTarget->Pixels;
    Uint32  dstStride = Graphics::CurrentRenderTarget->Width;
    Uint32* dstPxLine;

    int src_x;
    int src_y;
    int src_x1 = sx;
    int src_y1 = sy;
    int src_x2 = sx + sw - 1;
    int src_y2 = sy + sh - 1;

    int cos = CosTable[rotation];
    int sin = SinTable[rotation];
    int rcos = CosTable[(TRIG_TABLE_SIZE - rotation + TRIG_TABLE_SIZE) & TRIG_TABLE_MASK];
    int rsin = SinTable[(TRIG_TABLE_SIZE - rotation + TRIG_TABLE_SIZE) & TRIG_TABLE_MASK];

    int _x1 = offx;
    int _y1 = offy;
    int _x2 = offx + w;
    int _y2 = offy + h;

    switch (flipFlag) {
		case 1: _x1 = -offx - w; _x2 = -offx; break;
		case 2: _y1 = -offy - h; _y2 = -offy; break;
        case 3:
			_x1 = -offx - w; _x2 = -offx;
			_y1 = -offy - h; _y2 = -offy;
    }

    int dst_x1 = _x1;
    int dst_y1 = _y1;
    int dst_x2 = _x2;
    int dst_y2 = _y2;

    if (!Graphics::TextureBlend) {
        blendState.Mode = BlendMode_NORMAL;
        blendState.Opacity = 0xFF;
    }

    if (!SoftwareRenderer::AlterBlendState(blendState))
        return;

    int blendFlag = blendState.Mode;
    int opacity = blendState.Opacity;

    #define SET_MIN(a, b) if (a > b) a = b;
    #define SET_MAX(a, b) if (a < b) a = b;

    int px, py, cx, cy;

    py = _y1;
    {
        px = _x1;
        cx = (px * cos - py * sin); SET_MIN(dst_x1, cx); SET_MAX(dst_x2, cx);
        cy = (px * sin + py * cos); SET_MIN(dst_y1, cy); SET_MAX(dst_y2, cy);

        px = _x2;
        cx = (px * cos - py * sin); SET_MIN(dst_x1, cx); SET_MAX(dst_x2, cx);
        cy = (px * sin + py * cos); SET_MIN(dst_y1, cy); SET_MAX(dst_y2, cy);
    }

    py = _y2;
    {
        px = _x1;
        cx = (px * cos - py * sin); SET_MIN(dst_x1, cx); SET_MAX(dst_x2, cx);
        cy = (px * sin + py * cos); SET_MIN(dst_y1, cy); SET_MAX(dst_y2, cy);

        px = _x2;
        cx = (px * cos - py * sin); SET_MIN(dst_x1, cx); SET_MAX(dst_x2, cx);
        cy = (px * sin + py * cos); SET_MIN(dst_y1, cy); SET_MAX(dst_y2, cy);
    }

    #undef SET_MIN
    #undef SET_MAX

    dst_x1 >>= TRIG_TABLE_BITS;
    dst_y1 >>= TRIG_TABLE_BITS;
    dst_x2 >>= TRIG_TABLE_BITS;
    dst_y2 >>= TRIG_TABLE_BITS;

    dst_x1 += x;
    dst_y1 += y;
    dst_x2 += x + 1;
    dst_y2 += y + 1;

    int clip_x1 = 0,
        clip_y1 = 0,
        clip_x2 = 0,
        clip_y2 = 0;

    if (Graphics::CurrentClip.Enabled) {
        clip_x1 = Graphics::CurrentClip.X,
        clip_y1 = Graphics::CurrentClip.Y,
        clip_x2 = Graphics::CurrentClip.X + Graphics::CurrentClip.Width,
        clip_y2 = Graphics::CurrentClip.Y + Graphics::CurrentClip.Height;

        if (dst_x1 < clip_x1)
            dst_x1 = clip_x1;
        if (dst_y1 < clip_y1)
            dst_y1 = clip_y1;
        if (dst_x2 > clip_x2)
            dst_x2 = clip_x2;
        if (dst_y2 > clip_y2)
            dst_y2 = clip_y2;
    }
    else {
        clip_x2 = (int)Graphics::CurrentRenderTarget->Width,
        clip_y2 = (int)Graphics::CurrentRenderTarget->Height;

        if (dst_x1 < 0)
            dst_x1 = 0;
        if (dst_y1 < 0)
            dst_y1 = 0;
        if (dst_x2 > clip_x2)
            dst_x2 = clip_x2;
        if (dst_y2 > clip_y2)
            dst_y2 = clip_y2;
    }

    if (dst_x1 >= dst_x2)
        return;
    if (dst_y1 >= dst_y2)
        return;

    #define DEFORM_X { \
        dst_x += *deformValues; \
        if (dst_x < clip_x1) { \
            dst_x -= *deformValues; \
            continue; \
        } \
        if (dst_x >= clip_x2) { \
            dst_x -= *deformValues; \
            continue; \
        } \
    }

    #define DRAW_PLACEPIXEL(pixelFunction) \
        if ((color = srcPx[src_x + src_strideY]) & 0xFF000000U) \
            pixelFunction(&color, &dstPxLine[dst_x], blendState, multTableAt, multSubTableAt);
    #define DRAW_PLACEPIXEL_PAL(pixelFunction) \
        if ((color = srcPx[src_x + src_strideY])) \
            pixelFunction(&index[color], &dstPxLine[dst_x], blendState, multTableAt, multSubTableAt);

    #define DRAW_NOFLIP(pixelFunction, placePixelMacro) for (int dst_y = dst_y1, i_y = dst_y1 - y; dst_y < dst_y2; dst_y++, i_y++) { \
        i_y_rsin = -i_y * rsin; \
        i_y_rcos =  i_y * rcos; \
        dstPxLine = dstPx + dst_strideY; \
        index = &SoftwareRenderer::PaletteColors[SoftwareRenderer::PaletteIndexLines[dst_y]][0]; \
        if (SoftwareRenderer::UseSpriteDeform) \
            for (int dst_x = dst_x1, i_x = dst_x1 - x; dst_x < dst_x2; dst_x++, i_x++) { \
                DEFORM_X; \
                src_x = (i_x * rcos + i_y_rsin) >> TRIG_TABLE_BITS; \
                src_y = (i_x * rsin + i_y_rcos) >> TRIG_TABLE_BITS; \
                if (src_x >= _x1 && src_y >= _y1 && \
                    src_x <  _x2 && src_y <  _y2) { \
                    src_x       = (src_x1 + (src_x - _x1) * sw / w); \
                    src_strideY = (src_y1 + (src_y - _y1) * sh / h) * srcStride; \
                    placePixelMacro(pixelFunction); \
                } \
                dst_x -= *deformValues; \
            } \
        else \
            for (int dst_x = dst_x1, i_x = dst_x1 - x; dst_x < dst_x2; dst_x++, i_x++) { \
                src_x = (i_x * rcos + i_y_rsin) >> TRIG_TABLE_BITS; \
                src_y = (i_x * rsin + i_y_rcos) >> TRIG_TABLE_BITS; \
                if (src_x >= _x1 && src_y >= _y1 && \
                    src_x <  _x2 && src_y <  _y2) { \
                    src_x       = (src_x1 + (src_x - _x1) * sw / w); \
                    src_strideY = (src_y1 + (src_y - _y1) * sh / h) * srcStride; \
                    placePixelMacro(pixelFunction); \
                } \
            } \
        dst_strideY += dstStride; deformValues++; \
    }
    #define DRAW_FLIPX(pixelFunction, placePixelMacro) for (int dst_y = dst_y1, i_y = dst_y1 - y; dst_y < dst_y2; dst_y++, i_y++) { \
        i_y_rsin = -i_y * rsin; \
        i_y_rcos =  i_y * rcos; \
        dstPxLine = dstPx + dst_strideY; \
        index = &SoftwareRenderer::PaletteColors[SoftwareRenderer::PaletteIndexLines[dst_y]][0]; \
        if (SoftwareRenderer::UseSpriteDeform) \
            for (int dst_x = dst_x1, i_x = dst_x1 - x; dst_x < dst_x2; dst_x++, i_x++) { \
                DEFORM_X; \
                src_x = (i_x * rcos + i_y_rsin) >> TRIG_TABLE_BITS; \
                src_y = (i_x * rsin + i_y_rcos) >> TRIG_TABLE_BITS; \
                if (src_x >= _x1 && src_y >= _y1 && \
                    src_x <  _x2 && src_y <  _y2) { \
                    src_x       = (src_x2 - (src_x - _x1) * sw / w); \
                    src_strideY = (src_y1 + (src_y - _y1) * sh / h) * srcStride; \
                    placePixelMacro(pixelFunction); \
                } \
                dst_x -= *deformValues; \
            } \
        else \
            for (int dst_x = dst_x1, i_x = dst_x1 - x; dst_x < dst_x2; dst_x++, i_x++) { \
                src_x = (i_x * rcos + i_y_rsin) >> TRIG_TABLE_BITS; \
                src_y = (i_x * rsin + i_y_rcos) >> TRIG_TABLE_BITS; \
                if (src_x >= _x1 && src_y >= _y1 && \
                    src_x <  _x2 && src_y <  _y2) { \
                    src_x       = (src_x2 - (src_x - _x1) * sw / w); \
                    src_strideY = (src_y1 + (src_y - _y1) * sh / h) * srcStride; \
                    placePixelMacro(pixelFunction); \
                } \
            } \
        dst_strideY += dstStride; deformValues++; \
    }
    #define DRAW_FLIPY(pixelFunction, placePixelMacro) for (int dst_y = dst_y1, i_y = dst_y1 - y; dst_y < dst_y2; dst_y++, i_y++) { \
        i_y_rsin = -i_y * rsin; \
        i_y_rcos =  i_y * rcos; \
        dstPxLine = dstPx + dst_strideY; \
        index = &SoftwareRenderer::PaletteColors[SoftwareRenderer::PaletteIndexLines[dst_y]][0]; \
        if (SoftwareRenderer::UseSpriteDeform) \
            for (int dst_x = dst_x1, i_x = dst_x1 - x; dst_x < dst_x2; dst_x++, i_x++) { \
                DEFORM_X; \
                src_x = (i_x * rcos + i_y_rsin) >> TRIG_TABLE_BITS; \
                src_y = (i_x * rsin + i_y_rcos) >> TRIG_TABLE_BITS; \
                if (src_x >= _x1 && src_y >= _y1 && \
                    src_x <  _x2 && src_y <  _y2) { \
                    src_x       = (src_x1 + (src_x - _x1) * sw / w); \
                    src_strideY = (src_y2 - (src_y - _y1) * sh / h) * srcStride; \
                    placePixelMacro(pixelFunction); \
                } \
                dst_x -= *deformValues; \
            } \
        else \
            for (int dst_x = dst_x1, i_x = dst_x1 - x; dst_x < dst_x2; dst_x++, i_x++) { \
                src_x = (i_x * rcos + i_y_rsin) >> TRIG_TABLE_BITS; \
                src_y = (i_x * rsin + i_y_rcos) >> TRIG_TABLE_BITS; \
                if (src_x >= _x1 && src_y >= _y1 && \
                    src_x <  _x2 && src_y <  _y2) { \
                    src_x       = (src_x1 + (src_x - _x1) * sw / w); \
                    src_strideY = (src_y2 - (src_y - _y1) * sh / h) * srcStride; \
                    placePixelMacro(pixelFunction); \
                } \
            } \
        dst_strideY += dstStride; deformValues++; \
    }
    #define DRAW_FLIPXY(pixelFunction, placePixelMacro) for (int dst_y = dst_y1, i_y = dst_y1 - y; dst_y < dst_y2; dst_y++, i_y++) { \
        i_y_rsin = -i_y * rsin; \
        i_y_rcos =  i_y * rcos; \
        dstPxLine = dstPx + dst_strideY; \
        index = &SoftwareRenderer::PaletteColors[SoftwareRenderer::PaletteIndexLines[dst_y]][0]; \
        if (SoftwareRenderer::UseSpriteDeform) \
            for (int dst_x = dst_x1, i_x = dst_x1 - x; dst_x < dst_x2; dst_x++, i_x++) { \
                DEFORM_X; \
                src_x = (i_x * rcos + i_y_rsin) >> TRIG_TABLE_BITS; \
                src_y = (i_x * rsin + i_y_rcos) >> TRIG_TABLE_BITS; \
                if (src_x >= _x1 && src_y >= _y1 && \
                    src_x <  _x2 && src_y <  _y2) { \
                    src_x       = (src_x2 - (src_x - _x1) * sw / w); \
                    src_strideY = (src_y2 - (src_y - _y1) * sh / h) * srcStride; \
                    placePixelMacro(pixelFunction); \
                } \
                dst_x -= *deformValues; \
            } \
        else \
            for (int dst_x = dst_x1, i_x = dst_x1 - x; dst_x < dst_x2; dst_x++, i_x++) { \
                src_x = (i_x * rcos + i_y_rsin) >> TRIG_TABLE_BITS; \
                src_y = (i_x * rsin + i_y_rcos) >> TRIG_TABLE_BITS; \
                if (src_x >= _x1 && src_y >= _y1 && \
                    src_x <  _x2 && src_y <  _y2) { \
                    src_x       = (src_x2 - (src_x - _x1) * sw / w); \
                    src_strideY = (src_y2 - (src_y - _y1) * sh / h) * srcStride; \
                    placePixelMacro(pixelFunction); \
                } \
            } \
        dst_strideY += dstStride; deformValues++; \
    }

    #define BLENDFLAGS(flipMacro, placePixelMacro) \
        switch (blendFlag & (BlendFlag_MODE_MASK | BlendFlag_TINT_BIT)) { \
            SPRITE_PIXEL_NO_FILT_CASES(flipMacro, placePixelMacro); \
            SPRITE_PIXEL_TINT_CASES(flipMacro, placePixelMacro); \
        }

    if (blendFlag & (BlendFlag_TINT_BIT | BlendFlag_FILTER_BIT))
        SoftwareRenderer::SetTintFunction(blendFlag);

    Uint32 color;
    Uint32* index;
    int i_y_rsin, i_y_rcos;
    int dst_strideY, src_strideY;
    int* multTableAt = &SoftwareRenderer::MultTable[opacity << 8];
    int* multSubTableAt = &SoftwareRenderer::MultSubTable[opacity << 8];
    Sint32* deformValues = &SoftwareRenderer::SpriteDeformBuffer[dst_y1];

    if (Graphics::UsePalettes && texture->Paletted) {
        switch (flipFlag) {
            case 0:
                dst_strideY = dst_y1 * dstStride;
                BLENDFLAGS(DRAW_NOFLIP, DRAW_PLACEPIXEL_PAL);
                break;
            case 1:
                dst_strideY = dst_y1 * dstStride;
                BLENDFLAGS(DRAW_FLIPX, DRAW_PLACEPIXEL_PAL);
                break;
            case 2:
                dst_strideY = dst_y1 * dstStride;
                BLENDFLAGS(DRAW_FLIPY, DRAW_PLACEPIXEL_PAL);
                break;
            case 3:
                dst_strideY = dst_y1 * dstStride;
                BLENDFLAGS(DRAW_FLIPXY, DRAW_PLACEPIXEL_PAL);
                break;
        }
    }
    else {
        switch (flipFlag) {
            case 0:
                dst_strideY = dst_y1 * dstStride;
                BLENDFLAGS(DRAW_NOFLIP, DRAW_PLACEPIXEL);
                break;
            case 1:
                dst_strideY = dst_y1 * dstStride;
                BLENDFLAGS(DRAW_FLIPX, DRAW_PLACEPIXEL);
                break;
            case 2:
                dst_strideY = dst_y1 * dstStride;
                BLENDFLAGS(DRAW_FLIPY, DRAW_PLACEPIXEL);
                break;
            case 3:
                dst_strideY = dst_y1 * dstStride;
                BLENDFLAGS(DRAW_FLIPXY, DRAW_PLACEPIXEL);
                break;
        }
    }

    #undef DRAW_PLACEPIXEL
    #undef DRAW_PLACEPIXEL_PAL
    #undef DRAW_NOFLIP
    #undef DRAW_FLIPX
    #undef DRAW_FLIPY
    #undef DRAW_FLIPXY
    #undef BLENDFLAGS
}

PUBLIC STATIC void     SoftwareRenderer::DrawTexture(Texture* texture, float sx, float sy, float sw, float sh, float x, float y, float w, float h) {
    View* currentView = &Scene::Views[Scene::ViewCurrent];
    int cx = (int)std::floor(currentView->X);
    int cy = (int)std::floor(currentView->Y);

    Matrix4x4* out = Graphics::ModelViewMatrix;
    x += out->Values[12];
    y += out->Values[13];
    x -= cx;
    y -= cy;

    int textureWidth = texture->Width;
    int textureHeight = texture->Height;

    if (sw >= textureWidth - sx)
        sw  = textureWidth - sx;
    if (sh >= textureHeight - sy)
        sh  = textureHeight - sy;

    BlendState blendState = GetBlendState();
    if (sw != textureWidth || sh != textureHeight)
        DrawSpriteImageTransformed(texture, x, y, sx, sy, sw, sh, sx, sy, sw, sh, 0, 0, blendState);
    else
        DrawSpriteImage(texture, x, y, sw, sh, sx, sy, 0, blendState);
}
PUBLIC STATIC void     SoftwareRenderer::DrawSprite(ISprite* sprite, int animation, int frame, int x, int y, bool flipX, bool flipY, float scaleW, float scaleH, float rotation) {
    if (Graphics::SpriteRangeCheck(sprite, animation, frame)) return;

    AnimFrame frameStr = sprite->Animations[animation].Frames[frame];
    Texture* texture = sprite->Spritesheets[frameStr.SheetNumber];

    View* currentView = &Scene::Views[Scene::ViewCurrent];
    int cx = (int)std::floor(currentView->X);
    int cy = (int)std::floor(currentView->Y);

    Matrix4x4* out = Graphics::ModelViewMatrix;
    x += out->Values[12];
    y += out->Values[13];
    x -= cx;
    y -= cy;

    BlendState blendState = GetBlendState();
    int flipFlag = (int)flipX | ((int)flipY << 1);
    if (rotation != 0.0f || scaleW != 1.0f || scaleH != 1.0f) {
        int rot = (int)(rotation * TRIG_TABLE_HALF / M_PI) & TRIG_TABLE_MASK;
        DrawSpriteImageTransformed(texture,
            x, y,
            frameStr.OffsetX * scaleW, frameStr.OffsetY * scaleH,
            frameStr.Width * scaleW, frameStr.Height * scaleH,

            frameStr.X, frameStr.Y,
            frameStr.Width, frameStr.Height,
            flipFlag, rot,
            blendState);
        return;
    }
    switch (flipFlag) {
        case 0:
            DrawSpriteImage(texture,
                x + frameStr.OffsetX,
                y + frameStr.OffsetY,
                frameStr.Width, frameStr.Height, frameStr.X, frameStr.Y, flipFlag, blendState);
            break;
        case 1:
            DrawSpriteImage(texture,
                x - frameStr.OffsetX - frameStr.Width,
                y + frameStr.OffsetY,
                frameStr.Width, frameStr.Height, frameStr.X, frameStr.Y, flipFlag, blendState);
            break;
        case 2:
            DrawSpriteImage(texture,
                x + frameStr.OffsetX,
                y - frameStr.OffsetY - frameStr.Height,
                frameStr.Width, frameStr.Height, frameStr.X, frameStr.Y, flipFlag, blendState);
            break;
        case 3:
            DrawSpriteImage(texture,
                x - frameStr.OffsetX - frameStr.Width,
                y - frameStr.OffsetY - frameStr.Height,
                frameStr.Width, frameStr.Height, frameStr.X, frameStr.Y, flipFlag, blendState);
            break;
    }
}
PUBLIC STATIC void     SoftwareRenderer::DrawSpritePart(ISprite* sprite, int animation, int frame, int sx, int sy, int sw, int sh, int x, int y, bool flipX, bool flipY, float scaleW, float scaleH, float rotation) {
	if (Graphics::SpriteRangeCheck(sprite, animation, frame)) return;

    AnimFrame frameStr = sprite->Animations[animation].Frames[frame];
    Texture* texture = sprite->Spritesheets[frameStr.SheetNumber];

    View* currentView = &Scene::Views[Scene::ViewCurrent];
    int cx = (int)std::floor(currentView->X);
    int cy = (int)std::floor(currentView->Y);

    Matrix4x4* out = Graphics::ModelViewMatrix;
    x += out->Values[12];
    y += out->Values[13];
    x -= cx;
    y -= cy;

    if (sw >= frameStr.Width - sx)
        sw  = frameStr.Width - sx;
    if (sh >= frameStr.Height - sy)
        sh  = frameStr.Height - sy;

    BlendState blendState = GetBlendState();
    int flipFlag = (int)flipX | ((int)flipY << 1);
    if (rotation != 0.0f || scaleW != 1.0f || scaleH != 1.0f) {
        int rot = (int)(rotation * TRIG_TABLE_HALF / M_PI) & TRIG_TABLE_MASK;
        DrawSpriteImageTransformed(texture,
            x, y,
            (frameStr.OffsetX + sx) * scaleW, (frameStr.OffsetY + sy) * scaleH,
            sw * scaleW, sh * scaleH,

            frameStr.X + sx, frameStr.Y + sy,
            sw, sh,
            flipFlag, rot,
            blendState);
        return;
    }
    switch (flipFlag) {
        case 0:
            DrawSpriteImage(texture,
                x + frameStr.OffsetX + sx,
                y + frameStr.OffsetY + sy,
                sw, sh, frameStr.X + sx, frameStr.Y + sy, flipFlag, blendState);
            break;
        case 1:
            DrawSpriteImage(texture,
                x - frameStr.OffsetX - sw - sx,
                y + frameStr.OffsetY + sy,
                sw, sh, frameStr.X + sx, frameStr.Y + sy, flipFlag, blendState);
            break;
        case 2:
            DrawSpriteImage(texture,
                x + frameStr.OffsetX + sx,
                y - frameStr.OffsetY - sh - sy,
                sw, sh, frameStr.X + sx, frameStr.Y + sy, flipFlag, blendState);
            break;
        case 3:
            DrawSpriteImage(texture,
                x - frameStr.OffsetX - sw - sx,
                y - frameStr.OffsetY - sh - sy,
                sw, sh, frameStr.X + sx, frameStr.Y + sy, flipFlag, blendState);
            break;
    }
}

// Default Tile Display Line setup
PUBLIC STATIC void     SoftwareRenderer::DrawTile(int tile, int x, int y, bool flipX, bool flipY) {

}
PUBLIC STATIC void     SoftwareRenderer::DrawSceneLayer_InitTileScanLines(SceneLayer* layer, View* currentView) {
    switch (layer->DrawBehavior) {
        case DrawBehavior_PGZ1_BG:
        case DrawBehavior_HorizontalParallax: {
            int viewX = (int)currentView->X;
            int viewY = (int)currentView->Y;
            // int viewWidth = (int)currentView->Width;
            int viewHeight = (int)currentView->Height;
            int layerWidth = layer->Width * 16;
            int layerHeight = layer->Height * 16;
            int layerOffsetX = layer->OffsetX;
            int layerOffsetY = layer->OffsetY;

            // Set parallax positions
            ScrollingInfo* info = &layer->ScrollInfos[0];
            for (int i = 0; i < layer->ScrollInfoCount; i++) {
                info->Offset = Scene::Frame * info->ConstantParallax;
                info->Position = (info->Offset + ((viewX + layerOffsetX) * info->RelativeParallax)) >> 8;
                info->Position %= layerWidth;
                if (info->Position < 0)
                    info->Position += layerWidth;
                info++;
            }

            // Create scan lines
            Sint64 scrollOffset = Scene::Frame * layer->ConstantY;
            Sint64 scrollLine = (scrollOffset + ((viewY + layerOffsetY) * layer->RelativeY)) >> 8;
                   scrollLine %= layerHeight;
            if (scrollLine < 0)
                scrollLine += layerHeight;

            int* deformValues;
            Uint8* parallaxIndex;
            TileScanLine* scanLine;
            const int maxDeformLineMask = (MAX_DEFORM_LINES >> 1) - 1;

            scanLine = &TileScanLineBuffer[0];
            parallaxIndex = &layer->ScrollIndexes[scrollLine];
            deformValues = &layer->DeformSetA[(scrollLine + layer->DeformOffsetA) & maxDeformLineMask];
            for (int i = 0; i < layer->DeformSplitLine; i++) {
                // Set scan line start positions
                info = &layer->ScrollInfos[*parallaxIndex];
                scanLine->SrcX = info->Position;
                if (info->CanDeform)
                    scanLine->SrcX += *deformValues;
                scanLine->SrcX <<= 16;
                scanLine->SrcY = scrollLine << 16;

                scanLine->DeltaX = 0x10000;
                scanLine->DeltaY = 0x0000;

                // Iterate lines
                // NOTE: There is no protection from over-reading deform indexes past 512 here.
                scanLine++;
                scrollLine++;
                deformValues++;

                // If we've reach the last line of the layer, return to the first.
                if (scrollLine == layerHeight) {
                    scrollLine = 0;
                    parallaxIndex = &layer->ScrollIndexes[scrollLine];
                }
                else {
                    parallaxIndex++;
                }
            }

            deformValues = &layer->DeformSetB[(scrollLine + layer->DeformOffsetB) & maxDeformLineMask];
            for (int i = layer->DeformSplitLine; i < viewHeight; i++) {
                // Set scan line start positions
                info = &layer->ScrollInfos[*parallaxIndex];
                scanLine->SrcX = info->Position;
                if (info->CanDeform)
                    scanLine->SrcX += *deformValues;
                scanLine->SrcX <<= 16;
                scanLine->SrcY = scrollLine << 16;

                scanLine->DeltaX = 0x10000;
                scanLine->DeltaY = 0x0000;

                // Iterate lines
                // NOTE: There is no protection from over-reading deform indexes past 512 here.
                scanLine++;
                scrollLine++;
                deformValues++;

                // If we've reach the last line of the layer, return to the first.
                if (scrollLine == layerHeight) {
                    scrollLine = 0;
                    parallaxIndex = &layer->ScrollIndexes[scrollLine];
                }
                else {
                    parallaxIndex++;
                }
            }
            break;
        }
        case DrawBehavior_VerticalParallax: {
            break;
        }
        case DrawBehavior_CustomTileScanLines: {
            Sint64 scrollOffset = Scene::Frame * layer->ConstantY;
            Sint64 scrollPositionX = ((scrollOffset + (((int)currentView->X + layer->OffsetX) * layer->RelativeY)) >> 8);
                   scrollPositionX %= layer->Width * 16;
                   scrollPositionX <<= 16;
            Sint64 scrollPositionY = ((scrollOffset + (((int)currentView->Y + layer->OffsetY) * layer->RelativeY)) >> 8);
                   scrollPositionY %= layer->Height * 16;
                   scrollPositionY <<= 16;

            TileScanLine* scanLine = &TileScanLineBuffer[0];
            for (int i = 0; i < currentView->Height; i++) {
                scanLine->SrcX = scrollPositionX;
                scanLine->SrcY = scrollPositionY;
                scanLine->DeltaX = 0x10000;
                scanLine->DeltaY = 0x0;

                scrollPositionY += 0x10000;
                scanLine++;
            }

            break;
        }
    }
}

PUBLIC STATIC void     SoftwareRenderer::DrawSceneLayer_HorizontalParallax(SceneLayer* layer, View* currentView) {
    int dst_x1 = 0;
    int dst_y1 = 0;
    int dst_x2 = (int)Graphics::CurrentRenderTarget->Width;
    int dst_y2 = (int)Graphics::CurrentRenderTarget->Height;

    Uint32  srcStride = 0;

    Uint32* dstPx = (Uint32*)Graphics::CurrentRenderTarget->Pixels;
    Uint32  dstStride = Graphics::CurrentRenderTarget->Width;
    Uint32* dstPxLine;

    if (Graphics::CurrentClip.Enabled) {
        dst_x1 = Graphics::CurrentClip.X;
        dst_y1 = Graphics::CurrentClip.Y;
        dst_x2 = Graphics::CurrentClip.X + Graphics::CurrentClip.Width;
        dst_y2 = Graphics::CurrentClip.Y + Graphics::CurrentClip.Height;
    }

    if (dst_x1 >= dst_x2 || dst_y1 >= dst_y2)
        return;

    bool canCollide = (layer->Flags & SceneLayer::FLAGS_COLLIDEABLE);

    int layerWidthInBits = layer->WidthInBits;
    int layerWidthInPixels = layer->Width * 16;
    int layerWidth = layer->Width;
    int sourceTileCellX, sourceTileCellY;

    BlendState blendState = GetBlendState();

    if (!Graphics::TextureBlend) {
        blendState.Mode = BlendMode_NORMAL;
        blendState.Opacity = 0xFF;
    }

    if (!AlterBlendState(blendState))
        return;

    int blendFlag = blendState.Mode;
    int opacity = blendState.Opacity;
    if (blendFlag & (BlendFlag_TINT_BIT | BlendFlag_FILTER_BIT))
        SetTintFunction(blendFlag);

    int* multTableAt = &MultTable[opacity << 8];
    int* multSubTableAt = &MultSubTable[opacity << 8];

    Uint32* tile;
    Uint32* color;
    Uint32* index;
    int dst_strideY = dst_y1 * dstStride;

    int maxTileDraw = ((int)currentView->Stride >> 4) - 1;

    vector<Uint32> srcStrides;
    vector<Uint32*> tileSources;
    vector<Uint8> isPalettedSources;
    srcStrides.resize(Scene::TileSpriteInfos.size());
    tileSources.resize(Scene::TileSpriteInfos.size());
    isPalettedSources.resize(Scene::TileSpriteInfos.size());
    for (size_t i = 0; i < Scene::TileSpriteInfos.size(); i++) {
        TileSpriteInfo info = Scene::TileSpriteInfos[i];
        AnimFrame frameStr = info.Sprite->Animations[info.AnimationIndex].Frames[info.FrameIndex];
        Texture* texture = info.Sprite->Spritesheets[frameStr.SheetNumber];
        srcStrides[i] = srcStride = texture->Width;
        tileSources[i] = (&((Uint32*)texture->Pixels)[frameStr.X + frameStr.Y * srcStride]);
        isPalettedSources[i] = Graphics::UsePalettes && texture->Paletted;
    }

    Uint32 DRAW_COLLISION = 0;
    int c_pixelsOfTileRemaining, tileFlipOffset;
	TileConfig* baseTileCfg = Scene::ShowTileCollisionFlag == 2 ? Scene::TileCfgB : Scene::TileCfgA;

    void (*pixelFunction)(Uint32*, Uint32*, BlendState&, int*, int*) = NULL;
    if (blendFlag & BlendFlag_TINT_BIT)
        pixelFunction = PixelTintFunctions[blendFlag & BlendFlag_MODE_MASK];
    else
        pixelFunction = PixelNoFiltFunctions[blendFlag & BlendFlag_MODE_MASK];

    int j;
    TileScanLine* tScanLine = &TileScanLineBuffer[dst_y1];
    for (int dst_y = dst_y1; dst_y < dst_y2; dst_y++) {
        tScanLine->SrcX >>= 16;
        tScanLine->SrcY >>= 16;
        dstPxLine = dstPx + dst_strideY;

        if (tScanLine->SrcX < 0)
            tScanLine->SrcX += layerWidthInPixels;
        else if (tScanLine->SrcX >= layerWidthInPixels)
            tScanLine->SrcX -= layerWidthInPixels;

        int dst_x = dst_x1, c_dst_x = dst_x1;
        int pixelsOfTileRemaining;
        Sint64 srcX = tScanLine->SrcX, srcY = tScanLine->SrcY;
        index = &SoftwareRenderer::PaletteColors[SoftwareRenderer::PaletteIndexLines[dst_y]][0];

        // Draw leftmost tile in scanline
        int srcTX = srcX & 15;
        int srcTY = srcY & 15;
        sourceTileCellX = (srcX >> 4);
        sourceTileCellY = (srcY >> 4);
        c_pixelsOfTileRemaining = srcTX;
        pixelsOfTileRemaining = 16 - srcTX;
        tile = &layer->Tiles[sourceTileCellX + (sourceTileCellY << layerWidthInBits)];

        if ((*tile & TILE_IDENT_MASK) != Scene::EmptyTile) {
            int tileID = *tile & TILE_IDENT_MASK;

            if (Scene::ShowTileCollisionFlag && Scene::TileCfgA) {
                c_dst_x = dst_x;
                if (Scene::ShowTileCollisionFlag == 1)
                    DRAW_COLLISION = (*tile & TILE_COLLA_MASK) >> 28;
                else if (Scene::ShowTileCollisionFlag == 2)
                    DRAW_COLLISION = (*tile & TILE_COLLB_MASK) >> 26;

                switch (DRAW_COLLISION) {
                    case 1: DRAW_COLLISION = 0xFFFFFF00U; break;
                    case 2: DRAW_COLLISION = 0xFFFF0000U; break;
                    case 3: DRAW_COLLISION = 0xFFFFFFFFU; break;
                }
            }

            // If y-flipped
            if ((*tile & TILE_FLIPY_MASK))
                srcTY ^= 15;
            // If x-flipped
            if ((*tile & TILE_FLIPX_MASK)) {
                srcTX ^= 15;
                color = &tileSources[tileID][srcTX + srcTY * srcStrides[tileID]];
                if (isPalettedSources[tileID]) {
                    while (pixelsOfTileRemaining) {
                        if (*color)
                            pixelFunction(&index[*color], &dstPxLine[dst_x], blendState, multTableAt, multSubTableAt);
                        pixelsOfTileRemaining--;
                        dst_x++;
                        color--;
                    }
                }
                else {
                    while (pixelsOfTileRemaining) {
                        if (*color & 0xFF000000U)
                            pixelFunction(color, &dstPxLine[dst_x], blendState, multTableAt, multSubTableAt);
                        pixelsOfTileRemaining--;
                        dst_x++;
                        color--;
                    }
                }
            }
            // Otherwise
            else {
                color = &tileSources[tileID][srcTX + srcTY * srcStrides[tileID]];
                if (isPalettedSources[tileID]) {
                    while (pixelsOfTileRemaining) {
                        if (*color)
                            pixelFunction(&index[*color], &dstPxLine[dst_x], blendState, multTableAt, multSubTableAt);
                        pixelsOfTileRemaining--;
                        dst_x++;
                        color++;
                    }
                }
                else {
                    while (pixelsOfTileRemaining) {
                        if (*color & 0xFF000000U)
                            pixelFunction(color, &dstPxLine[dst_x], blendState, multTableAt, multSubTableAt);
                        pixelsOfTileRemaining--;
                        dst_x++;
                        color++;
                    }
                }
            }

            if (canCollide && DRAW_COLLISION) {
                tileFlipOffset = (
                    ( (!!(*tile & TILE_FLIPY_MASK)) << 1 ) | (!!(*tile & TILE_FLIPX_MASK))
                ) * Scene::TileCount;

                bool flipY = !!(*tile & TILE_FLIPY_MASK);
                bool isCeiling = !!baseTileCfg[tileID].IsCeiling;
                TileConfig* tile = (&baseTileCfg[tileID] + tileFlipOffset);
                for (int gg = c_pixelsOfTileRemaining; gg < 16; gg++) {
                    if ((flipY == isCeiling && (srcY & 15) >= tile->CollisionTop[gg] && tile->CollisionTop[gg] < 0xF0) ||
                        (flipY != isCeiling && (srcY & 15) <= tile->CollisionBottom[gg] && tile->CollisionBottom[gg] < 0xF0)) {
                        PixelNoFiltSetOpaque(&DRAW_COLLISION, &dstPxLine[c_dst_x], CurrentBlendState, NULL, NULL);
                    }
                    c_dst_x++;
                }
            }
        }
        else {
            dst_x += pixelsOfTileRemaining;
        }

        // Draw scanline tiles in batches of 16 pixels
        srcTY = srcY & 15;
        for (j = maxTileDraw; j; j--, dst_x += 16) {
            sourceTileCellX++;
            tile++;
            if (sourceTileCellX == layerWidth) {
                sourceTileCellX = 0;
                tile -= layerWidth;
            }

            if (Scene::ShowTileCollisionFlag && Scene::TileCfgA) {
                c_dst_x = dst_x;
                if (Scene::ShowTileCollisionFlag == 1)
                    DRAW_COLLISION = (*tile & TILE_COLLA_MASK) >> 28;
                else if (Scene::ShowTileCollisionFlag == 2)
                    DRAW_COLLISION = (*tile & TILE_COLLB_MASK) >> 26;

                switch (DRAW_COLLISION) {
                    case 1: DRAW_COLLISION = 0xFFFFFF00U; break;
                    case 2: DRAW_COLLISION = 0xFFFF0000U; break;
                    case 3: DRAW_COLLISION = 0xFFFFFFFFU; break;
                }
            }

            int srcTYb = srcTY;
            if ((*tile & TILE_IDENT_MASK) != Scene::EmptyTile) {
                int tileID = *tile & TILE_IDENT_MASK;
                // If y-flipped
                if ((*tile & TILE_FLIPY_MASK))
                    srcTYb ^= 15;
                // If x-flipped
                if ((*tile & TILE_FLIPX_MASK)) {
                    color = &tileSources[tileID][srcTYb * srcStrides[tileID]];
                    if (isPalettedSources[tileID]) {
                        #define UNLOOPED(n, k) if (color[n]) { pixelFunction(&index[color[n]], &dstPxLine[dst_x + k], blendState, multTableAt, multSubTableAt); }
                        UNLOOPED(0, 15);
                        UNLOOPED(1, 14);
                        UNLOOPED(2, 13);
                        UNLOOPED(3, 12);
                        UNLOOPED(4, 11);
                        UNLOOPED(5, 10);
                        UNLOOPED(6, 9);
                        UNLOOPED(7, 8);
                        UNLOOPED(8, 7);
                        UNLOOPED(9, 6);
                        UNLOOPED(10, 5);
                        UNLOOPED(11, 4);
                        UNLOOPED(12, 3);
                        UNLOOPED(13, 2);
                        UNLOOPED(14, 1);
                        UNLOOPED(15, 0);
                        #undef UNLOOPED
                    }
                    else {
                        #define UNLOOPED(n, k) if (color[n] & 0xFF000000U) { pixelFunction(&color[n], &dstPxLine[dst_x + k], blendState, multTableAt, multSubTableAt); }
                        UNLOOPED(0, 15);
                        UNLOOPED(1, 14);
                        UNLOOPED(2, 13);
                        UNLOOPED(3, 12);
                        UNLOOPED(4, 11);
                        UNLOOPED(5, 10);
                        UNLOOPED(6, 9);
                        UNLOOPED(7, 8);
                        UNLOOPED(8, 7);
                        UNLOOPED(9, 6);
                        UNLOOPED(10, 5);
                        UNLOOPED(11, 4);
                        UNLOOPED(12, 3);
                        UNLOOPED(13, 2);
                        UNLOOPED(14, 1);
                        UNLOOPED(15, 0);
                        #undef UNLOOPED
                    }
                }
                // Otherwise
                else {
                    color = &tileSources[tileID][srcTYb * srcStrides[tileID]];
                    if (isPalettedSources[tileID]) {
                        #define UNLOOPED(n, k) if (color[n]) { pixelFunction(&index[color[n]], &dstPxLine[dst_x + k], blendState, multTableAt, multSubTableAt); }
                        UNLOOPED(0, 0);
                        UNLOOPED(1, 1);
                        UNLOOPED(2, 2);
                        UNLOOPED(3, 3);
                        UNLOOPED(4, 4);
                        UNLOOPED(5, 5);
                        UNLOOPED(6, 6);
                        UNLOOPED(7, 7);
                        UNLOOPED(8, 8);
                        UNLOOPED(9, 9);
                        UNLOOPED(10, 10);
                        UNLOOPED(11, 11);
                        UNLOOPED(12, 12);
                        UNLOOPED(13, 13);
                        UNLOOPED(14, 14);
                        UNLOOPED(15, 15);
                        #undef UNLOOPED
                    }
                    else {
                        #define UNLOOPED(n, k) if (color[n] & 0xFF000000U) { pixelFunction(&color[n], &dstPxLine[dst_x + k], blendState, multTableAt, multSubTableAt); }
                        UNLOOPED(0, 0);
                        UNLOOPED(1, 1);
                        UNLOOPED(2, 2);
                        UNLOOPED(3, 3);
                        UNLOOPED(4, 4);
                        UNLOOPED(5, 5);
                        UNLOOPED(6, 6);
                        UNLOOPED(7, 7);
                        UNLOOPED(8, 8);
                        UNLOOPED(9, 9);
                        UNLOOPED(10, 10);
                        UNLOOPED(11, 11);
                        UNLOOPED(12, 12);
                        UNLOOPED(13, 13);
                        UNLOOPED(14, 14);
                        UNLOOPED(15, 15);
                        #undef UNLOOPED
                    }
                }

                if (canCollide && DRAW_COLLISION) {
                    tileFlipOffset = (
                        ( (!!(*tile & TILE_FLIPY_MASK)) << 1 ) | (!!(*tile & TILE_FLIPX_MASK))
                    ) * Scene::TileCount;

                    bool flipY = !!(*tile & TILE_FLIPY_MASK);
                    bool isCeiling = !!baseTileCfg[tileID].IsCeiling;
                    TileConfig* tile = (&baseTileCfg[tileID] + tileFlipOffset);
                    for (int gg = 0; gg < 16; gg++) {
                        if ((flipY == isCeiling && (srcY & 15) >= tile->CollisionTop[gg] && tile->CollisionTop[gg] < 0xF0) ||
                            (flipY != isCeiling && (srcY & 15) <= tile->CollisionBottom[gg] && tile->CollisionBottom[gg] < 0xF0)) {
                            PixelNoFiltSetOpaque(&DRAW_COLLISION, &dstPxLine[c_dst_x], CurrentBlendState, NULL, NULL);
                        }
                        c_dst_x++;
                    }
                }
            }
        }
        srcX += maxTileDraw * 16;

        tScanLine++;
        dst_strideY += dstStride;
    }
}
PUBLIC STATIC void     SoftwareRenderer::DrawSceneLayer_VerticalParallax(SceneLayer* layer, View* currentView) {

}
PUBLIC STATIC void     SoftwareRenderer::DrawSceneLayer_CustomTileScanLines(SceneLayer* layer, View* currentView) {
    int dst_x1 = 0;
    int dst_y1 = 0;
    int dst_x2 = (int)Graphics::CurrentRenderTarget->Width;
    int dst_y2 = (int)Graphics::CurrentRenderTarget->Height;

    // Uint32* srcPx = NULL;
    Uint32  srcStride = 0;
    // Uint32* srcPxLine;

    Uint32* dstPx = (Uint32*)Graphics::CurrentRenderTarget->Pixels;
    Uint32  dstStride = Graphics::CurrentRenderTarget->Width;
    Uint32* dstPxLine;

    if (Graphics::CurrentClip.Enabled) {
        dst_x1 = Graphics::CurrentClip.X;
        dst_y1 = Graphics::CurrentClip.Y;
        dst_x2 = Graphics::CurrentClip.X + Graphics::CurrentClip.Width;
        dst_y2 = Graphics::CurrentClip.Y + Graphics::CurrentClip.Height;
    }

    if (dst_x1 >= dst_x2 || dst_y1 >= dst_y2)
        return;

    int layerWidthInBits = layer->WidthInBits;
    int layerWidthTileMask = layer->WidthMask;
    int layerHeightTileMask = layer->HeightMask;
    int tile, sourceTileCellX, sourceTileCellY;
    TileSpriteInfo info;
    AnimFrame frameStr;
    Texture* texture;

    Uint32 color;
    Uint32* index;
    int dst_strideY = dst_y1 * dstStride;

    vector<Uint32> srcStrides;
    vector<Uint32*> tileSources;
    vector<Uint8> isPalettedSources;
    srcStrides.resize(Scene::TileSpriteInfos.size());
    tileSources.resize(Scene::TileSpriteInfos.size());
    isPalettedSources.resize(Scene::TileSpriteInfos.size());
    for (size_t i = 0; i < Scene::TileSpriteInfos.size(); i++) {
        info = Scene::TileSpriteInfos[i];
        frameStr = info.Sprite->Animations[info.AnimationIndex].Frames[info.FrameIndex];
        texture = info.Sprite->Spritesheets[frameStr.SheetNumber];
        srcStrides[i] = srcStride = texture->Width;
        tileSources[i] = (&((Uint32*)texture->Pixels)[frameStr.X + frameStr.Y * srcStride]);
        isPalettedSources[i] = Graphics::UsePalettes && texture->Paletted;
    }

    TileScanLine* scanLine = &TileScanLineBuffer[dst_y1];
    for (int dst_y = dst_y1; dst_y < dst_y2; dst_y++) {
        dstPxLine = dstPx + dst_strideY;

        Sint64 srcX = scanLine->SrcX,
               srcY = scanLine->SrcY,
               srcDX = scanLine->DeltaX,
               srcDY = scanLine->DeltaY;

        Uint32 maxHorzCells = scanLine->MaxHorzCells;
        Uint32 maxVertCells = scanLine->MaxVertCells;

        void (*linePixelFunction)(Uint32*, Uint32*, BlendState&, int*, int*) = NULL;

        BlendState blendState = GetBlendState();
        if (Graphics::TextureBlend) {
            blendState.Opacity -= 0xFF - scanLine->Opacity;
            if (blendState.Opacity < 0)
                blendState.Opacity = 0;
        }
        else {
            blendState.Mode = BlendFlag_OPAQUE;
            blendState.Opacity = 0;
        }

        int* multTableAt;
        int* multSubTableAt;
        int blendFlag;

        if (!AlterBlendState(blendState))
            goto scanlineDone;

        blendFlag = blendState.Mode;
        multTableAt = &MultTable[blendState.Opacity << 8];
        multSubTableAt = &MultSubTable[blendState.Opacity << 8];

        if (blendFlag & (BlendFlag_TINT_BIT | BlendFlag_FILTER_BIT)) {
            linePixelFunction = PixelTintFunctions[blendFlag & BlendFlag_MODE_MASK];
            SetTintFunction(blendFlag);
        }
        else
            linePixelFunction = PixelNoFiltFunctions[blendFlag & BlendFlag_MODE_MASK];

        index = &SoftwareRenderer::PaletteColors[SoftwareRenderer::PaletteIndexLines[dst_y]][0];

        for (int dst_x = dst_x1; dst_x < dst_x2; dst_x++) {
            int srcTX = srcX >> 16;
            int srcTY = srcY >> 16;

            sourceTileCellX = (srcX >> 20) & layerWidthTileMask;
            sourceTileCellY = (srcY >> 20) & layerHeightTileMask;

            if (maxHorzCells != 0)
                sourceTileCellX %= maxHorzCells;
            if (maxVertCells != 0)
                sourceTileCellY %= maxVertCells;

            tile = layer->Tiles[sourceTileCellX + (sourceTileCellY << layerWidthInBits)] & TILE_IDENT_MASK;
            if (tile != Scene::EmptyTile) {
                color = tileSources[tile][(srcTX & 15) + (srcTY & 15) * srcStrides[tile]];
                if (isPalettedSources[tile]) {
                    if (color)
                        linePixelFunction(&index[color], &dstPxLine[dst_x], blendState, multTableAt, multSubTableAt);
                }
                else {
                    if (color & 0xFF000000U)
                        linePixelFunction(&color, &dstPxLine[dst_x], blendState, multTableAt, multSubTableAt);
                }
            }
            srcX += srcDX;
            srcY += srcDY;
        }

scanlineDone:
        scanLine++;
        dst_strideY += dstStride;
    }
}
PUBLIC STATIC void     SoftwareRenderer::DrawSceneLayer(SceneLayer* layer, View* currentView) {
    if (layer->UsingCustomScanlineFunction && layer->DrawBehavior == DrawBehavior_CustomTileScanLines) {
        BytecodeObjectManager::Threads[0].RunFunction(&layer->CustomScanlineFunction, 0);
    }
    else {
        SoftwareRenderer::DrawSceneLayer_InitTileScanLines(layer, currentView);
    }

    switch (layer->DrawBehavior) {
        case DrawBehavior_PGZ1_BG:
		case DrawBehavior_HorizontalParallax:
			SoftwareRenderer::DrawSceneLayer_HorizontalParallax(layer, currentView);
			break;
		case DrawBehavior_VerticalParallax:
			SoftwareRenderer::DrawSceneLayer_VerticalParallax(layer, currentView);
			break;
		case DrawBehavior_CustomTileScanLines:
			SoftwareRenderer::DrawSceneLayer_CustomTileScanLines(layer, currentView);
			break;
	}
}

PUBLIC STATIC void     SoftwareRenderer::MakeFrameBufferID(ISprite* sprite, AnimFrame* frame) {
    frame->ID = 0;
}
