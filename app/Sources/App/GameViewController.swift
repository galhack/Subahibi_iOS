import GameController
import MetalKit
import UIKit

/// The game screen: an MTKView showing the engine's picture, and every kind
/// of input turned into the mouse and keyboard the game expects.
///
///   touch (iPhone, iPad, Vision Pro gaze + pinch)
///     tap                 left click
///     two-finger tap      right click (the game's menu / hides the text window)
///     swipe down / up     mouse wheel back / forward (the backlog)
///     press and hold      Ctrl while held (skip)
///   mouse or trackpad     move, left / right click and scroll as on a PC
///   keyboard              Return, Space, Escape, arrows, Ctrl, Page Up / Down ...
///   Apple TV remote       the touch surface moves a pointer, click = left click,
///                         Back = right click, up / down = wheel, Play/Pause = hold to skip
///   game controller       left stick = pointer, A = left, B = right click,
///                         right trigger = skip, d-pad up / down = wheel
final class GameViewController: UIViewController {
    private var renderer: Renderer!
    private var mtkView: MTKView { view as! MTKView }

    /// The pointer of the remote / a controller, in game pixels.
    private var virtualCursor = CGPoint(x: 640, y: 480)
    private var stickVelocity = CGVector.zero
    private var cursorLink: CADisplayLink?
    private var wheelAccumulator: CGFloat = 0
    private var holdingCtrl = false

    override func loadView() {
        view = MTKView(frame: .zero)
    }

    override func viewDidLoad() {
        super.viewDidLoad()
        guard let renderer = Renderer(view: mtkView) else {
            fatalError("Metal is not available")
        }
        self.renderer = renderer
        mtkView.delegate = renderer
        #if !os(tvOS)
        mtkView.isMultipleTouchEnabled = true
        #endif
        installGestures()
        installControllers()
        EngineBridge.presentMessageBox = { [weak self] text, caption, flags, answer in
            self?.presentMessageBox(text: text, caption: caption, flags: flags, answer: answer) ?? answer(-1)
        }
        #if os(tvOS)
        renderer.cursor = virtualCursor
        AppleHost_SetTouchInput(0)
        #endif
        let link = CADisplayLink(target: self, selector: #selector(stepCursor))
        link.add(to: .main, forMode: .common)
        cursorLink = link
    }

    deinit {
        cursorLink?.invalidate()
    }

    #if os(iOS)
    override var prefersStatusBarHidden: Bool { true }
    override var prefersHomeIndicatorAutoHidden: Bool { true }
    override var preferredScreenEdgesDeferringSystemGestures: UIRectEdge { .all }
    #endif

    // MARK: - touch, pointer, scroll

    private func installGestures() {
        #if os(tvOS)
        let pan = UIPanGestureRecognizer(target: self, action: #selector(remotePan(_:)))
        pan.allowedTouchTypes = [NSNumber(value: UITouch.TouchType.indirect.rawValue)]
        view.addGestureRecognizer(pan)
        #else
        let tap = UITapGestureRecognizer(target: self, action: #selector(tap(_:)))
        view.addGestureRecognizer(tap)

        let twoFinger = UITapGestureRecognizer(target: self, action: #selector(rightTap(_:)))
        twoFinger.numberOfTouchesRequired = 2
        view.addGestureRecognizer(twoFinger)

        let hold = UILongPressGestureRecognizer(target: self, action: #selector(hold(_:)))
        hold.minimumPressDuration = 0.6
        view.addGestureRecognizer(hold)

        let swipe = UIPanGestureRecognizer(target: self, action: #selector(swipe(_:)))
        swipe.maximumNumberOfTouches = 1
        view.addGestureRecognizer(swipe)
        tap.require(toFail: swipe)

        #if os(iOS)
        // A mouse or a trackpad (iPad, Mac) is handled as a real mouse in the touches
        // methods below - press, drag, release, either button - so the gestures take
        // fingers and the pencil only.
        let fingers = [UITouch.TouchType.direct, .pencil].map { NSNumber(value: $0.rawValue) }
        for r in [tap, twoFinger, hold, swipe] as [UIGestureRecognizer] {
            r.allowedTouchTypes = fingers
        }
        // the wheel and two-finger trackpad scrolling: scroll events only, no touches
        let scroll = UIPanGestureRecognizer(target: self, action: #selector(swipe(_:)))
        scroll.allowedScrollTypesMask = .all
        scroll.allowedTouchTypes = []
        view.addGestureRecognizer(scroll)

        let hover = UIHoverGestureRecognizer(target: self, action: #selector(hover(_:)))
        view.addGestureRecognizer(hover)
        #endif
        #endif
    }

    private func gamePoint(_ recognizer: UIGestureRecognizer) -> CGPoint {
        renderer.gamePoint(from: recognizer.location(in: view))
    }

    /// A touch has no hover: the game's "pointer suppressed" check ("B0 06")
    /// follows the last input, a pointer being whatever sent hover events lately.
    private var lastHover = Date.distantPast

    private func noteTouchOrPointer() {
        AppleHost_SetTouchInput(Date().timeIntervalSince(lastHover) < 2 ? 0 : 1)
    }

    @objc private func tap(_ r: UITapGestureRecognizer) {
        guard r.state == .ended else { return }
        renderer.cursor = nil
        virtualCursor = gamePoint(r)
        noteTouchOrPointer()
        click(button: 0, at: gamePoint(r))
    }

    @objc private func rightTap(_ r: UITapGestureRecognizer) {
        guard r.state == .ended else { return }
        click(button: 1, at: gamePoint(r))
    }

    @objc private func hold(_ r: UILongPressGestureRecognizer) {
        switch r.state {
        case .began: setCtrl(true)
        case .ended, .cancelled, .failed: setCtrl(false)
        default: break
        }
    }

    /// Vertical swipes and scrolls become wheel notches: 40 points per notch.
    @objc private func swipe(_ r: UIPanGestureRecognizer) {
        if r.state == .began { wheelAccumulator = 0 }
        let dy = r.translation(in: view).y
        r.setTranslation(.zero, in: view)
        wheelAccumulator += dy
        while abs(wheelAccumulator) >= 40 {
            let notch: Int32 = wheelAccumulator > 0 ? 120 : -120
            AppleHost_Wheel(notch)
            wheelAccumulator -= wheelAccumulator > 0 ? 40 : -40
        }
    }

    #if os(iOS)
    @objc private func hover(_ r: UIHoverGestureRecognizer) {
        let p = gamePoint(r)
        lastHover = Date()
        AppleHost_SetTouchInput(0)
        AppleHost_PointerMove(Int32(p.x), Int32(p.y))
    }
    #endif

    #if os(iOS)
    // MARK: - mouse and trackpad: real presses

    private var pointerButton: Int32?

    private func pointerTouch(_ touches: Set<UITouch>) -> UITouch? {
        touches.first { $0.type == .indirectPointer }
    }

    override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent?) {
        guard let t = pointerTouch(touches), pointerButton == nil else { return super.touchesBegan(touches, with: event) }
        let button: Int32 = event?.buttonMask.contains(.secondary) == true ? 1 : 0
        let p = renderer.gamePoint(from: t.location(in: view))
        lastHover = Date()
        renderer.cursor = nil
        AppleHost_SetTouchInput(0)
        AppleHost_PointerMove(Int32(p.x), Int32(p.y))
        AppleHost_PointerButton(button, 1, Int32(p.x), Int32(p.y))
        pointerButton = button
    }

    override func touchesMoved(_ touches: Set<UITouch>, with event: UIEvent?) {
        guard let t = pointerTouch(touches), pointerButton != nil else { return super.touchesMoved(touches, with: event) }
        let p = renderer.gamePoint(from: t.location(in: view))
        AppleHost_PointerMove(Int32(p.x), Int32(p.y))
    }

    override func touchesEnded(_ touches: Set<UITouch>, with event: UIEvent?) {
        guard let t = pointerTouch(touches), let button = pointerButton else { return super.touchesEnded(touches, with: event) }
        let p = renderer.gamePoint(from: t.location(in: view))
        AppleHost_PointerButton(button, 0, Int32(p.x), Int32(p.y))
        pointerButton = nil
    }

    override func touchesCancelled(_ touches: Set<UITouch>, with event: UIEvent?) {
        touchesEnded(touches, with: event)
    }
    #endif

    private func click(button: Int32, at p: CGPoint) {
        let x = Int32(p.x), y = Int32(p.y)
        fputs("input: click \(button) at \(x),\(y) (view \(view.bounds.size), picture \(renderer.contentRect))\n", stderr)
        AppleHost_PointerMove(x, y)
        AppleHost_PointerButton(button, 1, x, y)
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.05) {
            AppleHost_PointerButton(button, 0, x, y)
        }
    }

    private func setCtrl(_ down: Bool) {
        guard down != holdingCtrl else { return }
        holdingCtrl = down
        AppleHost_Key(VK.control, down ? 1 : 0)
    }

    // MARK: - the Apple TV remote and game controllers: a virtual pointer

    #if os(tvOS)
    @objc private func remotePan(_ r: UIPanGestureRecognizer) {
        let t = r.translation(in: view)
        r.setTranslation(.zero, in: view)
        moveCursor(by: CGVector(dx: t.x * 1.6, dy: t.y * 1.6))
    }
    #endif

    private func moveCursor(by delta: CGVector) {
        let size = renderer.gameSize
        virtualCursor.x = min(max(virtualCursor.x + delta.dx, 0), size.width - 1)
        virtualCursor.y = min(max(virtualCursor.y + delta.dy, 0), size.height - 1)
        renderer.cursor = virtualCursor
        AppleHost_PointerMove(Int32(virtualCursor.x), Int32(virtualCursor.y))
    }

    @objc private func stepCursor(_ link: CADisplayLink) {
        guard stickVelocity != .zero else { return }
        let speed: CGFloat = 900 // game pixels per second at full deflection
        moveCursor(by: CGVector(dx: stickVelocity.dx * speed * link.duration, dy: -stickVelocity.dy * speed * link.duration))
    }

    private func installControllers() {
        NotificationCenter.default.addObserver(forName: .GCControllerDidConnect, object: nil, queue: .main) { [weak self] note in
            if let c = note.object as? GCController { self?.attach(c) }
        }
        GCController.controllers().forEach(attach)
    }

    private func attach(_ controller: GCController) {
        guard let pad = controller.extendedGamepad else { return }
        pad.leftThumbstick.valueChangedHandler = { [weak self] _, x, y in
            let dead: Float = 0.15
            self?.stickVelocity = CGVector(dx: CGFloat(abs(x) > dead ? x : 0), dy: CGFloat(abs(y) > dead ? y : 0))
            if self?.stickVelocity != .zero { self?.showCursor() }
        }
        pad.buttonA.pressedChangedHandler = { [weak self] _, _, pressed in self?.cursorButton(0, pressed) }
        pad.buttonB.pressedChangedHandler = { [weak self] _, _, pressed in self?.cursorButton(1, pressed) }
        pad.rightTrigger.pressedChangedHandler = { [weak self] _, _, pressed in self?.setCtrl(pressed) }
        pad.dpad.up.pressedChangedHandler = { _, _, pressed in if pressed { AppleHost_Wheel(120) } }
        pad.dpad.down.pressedChangedHandler = { _, _, pressed in if pressed { AppleHost_Wheel(-120) } }
    }

    /// The pointer appears once a controller or the remote is used, and goes away with a touch.
    private func showCursor() {
        if renderer.cursor == nil { renderer.cursor = virtualCursor }
    }

    private func cursorButton(_ button: Int32, _ down: Bool) {
        showCursor()
        AppleHost_SetTouchInput(0)
        AppleHost_PointerButton(button, down ? 1 : 0, Int32(virtualCursor.x), Int32(virtualCursor.y))
    }

    // MARK: - keys and the remote's buttons

    override func pressesBegan(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
        var handled = false
        for press in presses {
            #if os(tvOS)
            switch press.type {
            case .select: cursorButton(0, true); handled = true; continue
            case .menu: cursorButton(1, true); handled = true; continue
            case .playPause: setCtrl(true); handled = true; continue
            case .upArrow: AppleHost_Wheel(120); handled = true; continue
            case .downArrow: AppleHost_Wheel(-120); handled = true; continue
            default: break
            }
            #endif
            if let key = press.key, let vk = VK.from(key.keyCode) {
                AppleHost_Key(vk, 1)
                handled = true
            }
        }
        if !handled { super.pressesBegan(presses, with: event) }
    }

    override func pressesEnded(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
        var handled = false
        for press in presses {
            #if os(tvOS)
            switch press.type {
            case .select: cursorButton(0, false); handled = true; continue
            case .menu: cursorButton(1, false); handled = true; continue
            case .playPause: setCtrl(false); handled = true; continue
            case .upArrow, .downArrow: handled = true; continue
            default: break
            }
            #endif
            if let key = press.key, let vk = VK.from(key.keyCode) {
                AppleHost_Key(vk, 0)
                handled = true
            }
        }
        if !handled { super.pressesEnded(presses, with: event) }
    }

    override func pressesCancelled(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
        pressesEnded(presses, with: event)
    }

    // MARK: - the engine's message boxes

    private func presentMessageBox(text: String, caption: String, flags: UInt32, answer: @escaping (Int32) -> Void) {
        let alert = UIAlertController(title: caption, message: text, preferredStyle: .alert)
        for button in MessageBoxButtons.buttons(for: flags) {
            alert.addAction(UIAlertAction(title: button.title, style: .default) { _ in answer(button.id) })
        }
        present(alert, animated: true)
    }
}

/// Windows virtual-key codes of the keys a player may press.
enum VK {
    static let control: Int32 = 0x11

    static func from(_ code: UIKeyboardHIDUsage) -> Int32? {
        switch code {
        case .keyboardReturnOrEnter, .keypadEnter: return 0x0D
        case .keyboardSpacebar: return 0x20
        case .keyboardEscape: return 0x1B
        case .keyboardTab: return 0x09
        case .keyboardDeleteOrBackspace: return 0x08
        case .keyboardLeftArrow: return 0x25
        case .keyboardUpArrow: return 0x26
        case .keyboardRightArrow: return 0x27
        case .keyboardDownArrow: return 0x28
        case .keyboardPageUp: return 0x21
        case .keyboardPageDown: return 0x22
        case .keyboardHome: return 0x24
        case .keyboardEnd: return 0x23
        case .keyboardLeftControl, .keyboardRightControl: return 0x11
        case .keyboardLeftShift, .keyboardRightShift: return 0x10
        case .keyboardLeftAlt, .keyboardRightAlt: return 0x12
        default: break
        }
        let raw = code.rawValue
        if raw >= UIKeyboardHIDUsage.keyboardA.rawValue && raw <= UIKeyboardHIDUsage.keyboardZ.rawValue {
            return Int32(0x41 + raw - UIKeyboardHIDUsage.keyboardA.rawValue)
        }
        if raw >= UIKeyboardHIDUsage.keyboard1.rawValue && raw <= UIKeyboardHIDUsage.keyboard9.rawValue {
            return Int32(0x31 + raw - UIKeyboardHIDUsage.keyboard1.rawValue)
        }
        if code == .keyboard0 { return 0x30 }
        if raw >= UIKeyboardHIDUsage.keyboardF1.rawValue && raw <= UIKeyboardHIDUsage.keyboardF12.rawValue {
            return Int32(0x70 + raw - UIKeyboardHIDUsage.keyboardF1.rawValue)
        }
        return nil
    }
}
