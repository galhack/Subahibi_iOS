import CoreGraphics
import Metal
import MetalKit

/// Draws the engine's window image (and the movie playing over it) into an
/// MTKView, scaled to fit with black bars.  The image is uploaded only when
/// the engine presented something new, so a still scene costs nothing.
final class Renderer: NSObject, MTKViewDelegate {
    private let device: MTLDevice
    private let queue: MTLCommandQueue
    private let opaquePipeline: MTLRenderPipelineState
    private let blendPipeline: MTLRenderPipelineState
    private let linear: MTLSamplerState

    private var gameTexture: MTLTexture?
    private var movieTexture: MTLTexture?
    private var cursorTexture: MTLTexture?
    private var frameSerial: UInt32 = 0
    private var movieSerial: UInt32 = 0
    private var movieRect = CGRect.zero // in game pixels
    private var movieShown = false
    private var needsRedraw = true

    /// The game's client size (1280 x 960 for this game once the engine created its window).
    private(set) var gameSize = CGSize(width: 1280, height: 960)
    /// The picture's rectangle in the view, in points (for mapping input).
    private(set) var contentRect = CGRect.zero

    /// The pointer drawn by the renderer (Apple TV remote, game controllers), in game pixels; nil hides it.
    var cursor: CGPoint? {
        didSet { if cursor != oldValue { needsRedraw = true } }
    }

    init?(view: MTKView) {
        guard let device = MTLCreateSystemDefaultDevice(), let queue = device.makeCommandQueue(),
              let library = try? device.makeLibrary(source: Self.shaderSource, options: nil) else { return nil }
        self.device = device
        self.queue = queue
        view.device = device
        view.colorPixelFormat = .bgra8Unorm
        view.framebufferOnly = true
        view.clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
        view.preferredFramesPerSecond = 60

        let desc = MTLRenderPipelineDescriptor()
        desc.vertexFunction = library.makeFunction(name: "quad_vertex")
        desc.fragmentFunction = library.makeFunction(name: "opaque_fragment")
        desc.colorAttachments[0].pixelFormat = view.colorPixelFormat
        let blend = MTLRenderPipelineDescriptor()
        blend.vertexFunction = desc.vertexFunction
        blend.fragmentFunction = library.makeFunction(name: "blend_fragment")
        blend.colorAttachments[0].pixelFormat = view.colorPixelFormat
        blend.colorAttachments[0].isBlendingEnabled = true
        blend.colorAttachments[0].sourceRGBBlendFactor = .one
        blend.colorAttachments[0].destinationRGBBlendFactor = .oneMinusSourceAlpha
        blend.colorAttachments[0].sourceAlphaBlendFactor = .one
        blend.colorAttachments[0].destinationAlphaBlendFactor = .oneMinusSourceAlpha
        guard let opaque = try? device.makeRenderPipelineState(descriptor: desc),
              let blended = try? device.makeRenderPipelineState(descriptor: blend) else { return nil }
        opaquePipeline = opaque
        blendPipeline = blended

        let samp = MTLSamplerDescriptor()
        samp.minFilter = .linear
        samp.magFilter = .linear
        samp.sAddressMode = .clampToEdge
        samp.tAddressMode = .clampToEdge
        guard let sampler = device.makeSamplerState(descriptor: samp) else { return nil }
        linear = sampler
        super.init()
        cursorTexture = Self.makeCursor(device: device)
    }

    func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {
        needsRedraw = true
    }

    /// A view point to game pixels, clamped to the picture.
    func gamePoint(from viewPoint: CGPoint) -> CGPoint {
        guard contentRect.width > 0, contentRect.height > 0 else { return .zero }
        let x = (viewPoint.x - contentRect.minX) / contentRect.width * gameSize.width
        let y = (viewPoint.y - contentRect.minY) / contentRect.height * gameSize.height
        return CGPoint(x: min(max(x, 0), gameSize.width - 1), y: min(max(y, 0), gameSize.height - 1))
    }

    func draw(in view: MTKView) {
        let uploaded = uploadFrame()
        let movie = uploadMovie()
        let scale = view.contentScaleFactor
        let drawable = view.drawableSize
        let fit = Self.fitRect(content: gameSize, in: drawable)
        contentRect = CGRect(x: fit.minX / scale, y: fit.minY / scale, width: fit.width / scale, height: fit.height / scale)
        guard uploaded || movie || needsRedraw else { return }
        guard let pass = view.currentRenderPassDescriptor, let target = view.currentDrawable,
              let commands = queue.makeCommandBuffer(),
              let encoder = commands.makeRenderCommandEncoder(descriptor: pass) else { return }
        needsRedraw = false
        if let tex = gameTexture {
            encoder.setRenderPipelineState(opaquePipeline)
            draw(texture: tex, in: fit, drawable: drawable, encoder: encoder)
        }
        if movieShown, let tex = movieTexture {
            let r = CGRect(x: fit.minX + movieRect.minX / gameSize.width * fit.width,
                           y: fit.minY + movieRect.minY / gameSize.height * fit.height,
                           width: movieRect.width / gameSize.width * fit.width,
                           height: movieRect.height / gameSize.height * fit.height)
            encoder.setRenderPipelineState(opaquePipeline)
            draw(texture: tex, in: r, drawable: drawable, encoder: encoder)
        }
        if let c = cursor, let tex = cursorTexture {
            let size = 40.0 * Double(scale) * max(1.0, Double(fit.height) / 1080.0)
            let p = CGPoint(x: fit.minX + c.x / gameSize.width * fit.width, y: fit.minY + c.y / gameSize.height * fit.height)
            encoder.setRenderPipelineState(blendPipeline)
            draw(texture: tex, in: CGRect(x: p.x, y: p.y, width: size, height: size), drawable: drawable, encoder: encoder)
        }
        encoder.endEncoding()
        commands.present(target)
        commands.commit()
    }

    private func draw(texture: MTLTexture, in rect: CGRect, drawable: CGSize, encoder: MTLRenderCommandEncoder) {
        var ndc = SIMD4<Float>(Float(rect.minX / drawable.width * 2 - 1), Float(1 - rect.minY / drawable.height * 2),
                               Float(rect.maxX / drawable.width * 2 - 1), Float(1 - rect.maxY / drawable.height * 2))
        encoder.setVertexBytes(&ndc, length: MemoryLayout<SIMD4<Float>>.size, index: 0)
        encoder.setFragmentTexture(texture, index: 0)
        encoder.setFragmentSamplerState(linear, index: 0)
        encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
    }

    /// The engine's window image, when it changed since the last upload.
    private func uploadFrame() -> Bool {
        var frame = AppleHostFrame_t()
        guard AppleHost_LockFrame(frameSerial, &frame) == 1 else { return false }
        defer { AppleHost_UnlockFrame() }
        let w = Int(frame.width), h = Int(frame.height)
        guard w > 0, h > 0, let pixels = frame.pixels else { return false }
        if gameTexture?.width != w || gameTexture?.height != h {
            gameTexture = makeTexture(width: w, height: h)
            gameSize = CGSize(width: w, height: h)
        }
        gameTexture?.replace(region: MTLRegionMake2D(0, 0, w, h), mipmapLevel: 0, withBytes: pixels,
                             bytesPerRow: Int(frame.pitch))
        frameSerial = frame.serial
        return true
    }

    /// The movie's frame, when it changed; drops the overlay when the movie is over.
    private func uploadMovie() -> Bool {
        var movie = AppleHostMovieFrame_t()
        var playing: Int32 = 0
        let locked = AppleHost_LockMovieFrame(movieSerial, &movie, &playing) == 1
        if !locked {
            if playing == 0 && movieShown {
                movieShown = false
                return true
            }
            return false
        }
        defer { AppleHost_UnlockMovieFrame() }
        let w = Int(movie.width), h = Int(movie.height)
        guard w > 0, h > 0, let pixels = movie.pixels else { return false }
        if movieTexture?.width != w || movieTexture?.height != h {
            movieTexture = makeTexture(width: w, height: h)
        }
        movieTexture?.replace(region: MTLRegionMake2D(0, 0, w, h), mipmapLevel: 0, withBytes: pixels,
                              bytesPerRow: Int(movie.pitch))
        movieRect = CGRect(x: Int(movie.x), y: Int(movie.y), width: Int(movie.w), height: Int(movie.h))
        movieSerial = movie.serial
        movieShown = true
        return true
    }

    private func makeTexture(width: Int, height: Int) -> MTLTexture? {
        let desc = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .bgra8Unorm, width: width, height: height,
                                                            mipmapped: false)
        desc.usage = .shaderRead
        #if os(macOS)
        desc.storageMode = .managed
        #else
        desc.storageMode = .shared
        #endif
        return device.makeTexture(descriptor: desc)
    }

    /// The largest rectangle of the content's aspect inside `size`, centred.
    static func fitRect(content: CGSize, in size: CGSize) -> CGRect {
        guard content.width > 0, content.height > 0, size.width > 0, size.height > 0 else { return .zero }
        let scale = min(size.width / content.width, size.height / content.height)
        let w = (content.width * scale).rounded(), h = (content.height * scale).rounded()
        return CGRect(x: ((size.width - w) / 2).rounded(), y: ((size.height - h) / 2).rounded(), width: w, height: h)
    }

    /// The shaders, compiled when the renderer starts (no Metal toolchain is
    /// needed to build the project).
    private static let shaderSource = """
#include <metal_stdlib>
using namespace metal;

// A textured rectangle: `rect` is (left, top, right, bottom) in normalised device coordinates.
struct QuadOut
{
	float4 position [[position]];
	float2 uv;
};

vertex QuadOut quad_vertex(uint vid [[vertex_id]], constant float4& rect [[buffer(0)]])
{
	float2 uv = float2((vid & 1) ? 1.0 : 0.0, (vid & 2) ? 1.0 : 0.0);
	QuadOut o;
	o.position = float4(mix(rect.x, rect.z, uv.x), mix(rect.y, rect.w, uv.y), 0.0, 1.0);
	o.uv = uv;
	return o;
}

// The engine's picture: 0x00RRGGBB words, read as BGRA with an alpha the engine leaves at 0.
fragment float4 opaque_fragment(QuadOut in [[stage_in]], texture2d<float> tex [[texture(0)]], sampler smp [[sampler(0)]])
{
	return float4(tex.sample(smp, in.uv).rgb, 1.0);
}

// The cursor of the remote / a game controller: premultiplied alpha.
fragment float4 blend_fragment(QuadOut in [[stage_in]], texture2d<float> tex [[texture(0)]], sampler smp [[sampler(0)]])
{
	return tex.sample(smp, in.uv);
}
"""

    /// An arrow pointer drawn with Core Graphics (premultiplied BGRA).
    private static func makeCursor(device: MTLDevice) -> MTLTexture? {
        let size = 64
        guard let ctx = CGContext(data: nil, width: size, height: size, bitsPerComponent: 8, bytesPerRow: size * 4,
                                  space: CGColorSpaceCreateDeviceRGB(),
                                  bitmapInfo: CGImageAlphaInfo.premultipliedFirst.rawValue
                                      | CGBitmapInfo.byteOrder32Little.rawValue) else { return nil }
        ctx.translateBy(x: 0, y: CGFloat(size))
        ctx.scaleBy(x: 1, y: -1)
        let path = CGMutablePath()
        path.move(to: CGPoint(x: 4, y: 4))
        path.addLine(to: CGPoint(x: 4, y: 52))
        path.addLine(to: CGPoint(x: 16, y: 40))
        path.addLine(to: CGPoint(x: 26, y: 60))
        path.addLine(to: CGPoint(x: 34, y: 56))
        path.addLine(to: CGPoint(x: 24, y: 37))
        path.addLine(to: CGPoint(x: 40, y: 37))
        path.closeSubpath()
        ctx.setShadow(offset: CGSize(width: 2, height: -2), blur: 4, color: CGColor(gray: 0, alpha: 0.6))
        ctx.addPath(path)
        ctx.setFillColor(CGColor(gray: 1, alpha: 1))
        ctx.fillPath()
        ctx.setShadow(offset: .zero, blur: 0, color: nil)
        ctx.addPath(path)
        ctx.setStrokeColor(CGColor(gray: 0, alpha: 1))
        ctx.setLineWidth(3)
        ctx.strokePath()
        guard let data = ctx.data else { return nil }
        let desc = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .bgra8Unorm, width: size, height: size, mipmapped: false)
        desc.usage = .shaderRead
        let tex = device.makeTexture(descriptor: desc)
        tex?.replace(region: MTLRegionMake2D(0, 0, size, size), mipmapLevel: 0, withBytes: data, bytesPerRow: size * 4)
        return tex
    }
}
