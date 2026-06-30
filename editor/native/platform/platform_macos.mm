#include "platform.hpp"

#include <json.hpp>

#import <Cocoa/Cocoa.h>
#import <CoreVideo/CoreVideo.h>
#import <QuartzCore/CAMetalLayer.h>
#import <WebKit/WebKit.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>

// This platform shim deliberately uses a handful of APIs deprecated in recent
// macOS SDKs (CVDisplayLink, WKWebView.javaScriptEnabled, NSSavePanel.
// allowedFileTypes). They still function and the replacements (NSView display
// links / UTType content types) are a larger, riskier refactor than warrants
// doing inline — silence the deprecation noise for this TU.
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

// WKWebView subclass that accepts dragged 3D-asset files (glTF / USD) anywhere
// over the editor and forwards their real filesystem paths to the frontend as a
// "menu-drop-import" broadcast — the same channel File→Import uses. We intercept
// at the native layer (rather than HTML5 drop) because the WebView's File object
// hides the on-disk path, and USD needs it to resolve textures / sublayers
// relative to the file. Non-asset drags fall through to WKWebView's defaults.
@interface TraceyWebView : WKWebView
@end

@implementation TraceyWebView

static BOOL traceyIsSupportedAsset(NSURL* url) {
    static NSSet<NSString*>* exts = nil;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        exts = [NSSet setWithArray:@[ @"gltf", @"glb", @"usd", @"usda", @"usdc", @"usdz" ]];
    });
    return url && [exts containsObject:url.pathExtension.lowercaseString];
}

- (instancetype)initWithFrame:(NSRect)frame configuration:(WKWebViewConfiguration*)configuration {
    self = [super initWithFrame:frame configuration:configuration];
    if (self) [self registerForDraggedTypes:@[ NSPasteboardTypeFileURL ]];
    return self;
}

- (NSArray<NSURL*>*)traceySupportedURLs:(id<NSDraggingInfo>)sender {
    NSArray* urls = [[sender draggingPasteboard]
        readObjectsForClasses:@[ [NSURL class] ]
                      options:@{ NSPasteboardURLReadingFileURLsOnlyKey : @YES }];
    NSMutableArray<NSURL*>* out = [NSMutableArray array];
    for (NSURL* u in urls)
        if (traceyIsSupportedAsset(u)) [out addObject:u];
    return out;
}

- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender {
    if ([self traceySupportedURLs:sender].count > 0) return NSDragOperationCopy;
    return [super draggingEntered:sender];
}

- (NSDragOperation)draggingUpdated:(id<NSDraggingInfo>)sender {
    if ([self traceySupportedURLs:sender].count > 0) return NSDragOperationCopy;
    return [super draggingUpdated:sender];
}

- (BOOL)prepareForDragOperation:(id<NSDraggingInfo>)sender {
    if ([self traceySupportedURLs:sender].count > 0) return YES;
    return [super prepareForDragOperation:sender];
}

- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender {
    NSArray<NSURL*>* urls = [self traceySupportedURLs:sender];
    if (urls.count == 0) return [super performDragOperation:sender];
    NSMutableArray<NSString*>* paths = [NSMutableArray array];
    for (NSURL* u in urls) [paths addObject:u.path];
    NSData* msgData = [NSJSONSerialization
        dataWithJSONObject:@{ @"event" : @"menu-drop-import", @"paths" : paths }
                   options:0
                     error:nil];
    if (!msgData) return YES;
    NSString* msgStr = [[NSString alloc] initWithData:msgData encoding:NSUTF8StringEncoding];
    // JS-escape the message JSON as a string literal for __traceyBroadcast(...).
    NSData* litData = [NSJSONSerialization dataWithJSONObject:msgStr
                                                     options:NSJSONWritingFragmentsAllowed
                                                       error:nil];
    NSString* lit = [[NSString alloc] initWithData:litData encoding:NSUTF8StringEncoding];
    NSString* js = [NSString
        stringWithFormat:@"if(window.__traceyBroadcast) window.__traceyBroadcast(%@)", lit];
    [self evaluateJavaScript:js completionHandler:nil];
    return YES;
}

@end

@interface TraceyNSWindow : NSWindow
@property(nonatomic, assign) bool* closeFlag;
@property(nonatomic, assign) WKWebView* editorWebView;
@end

@implementation TraceyNSWindow
- (BOOL)canBecomeKeyWindow {
    return YES;
}
- (BOOL)canBecomeMainWindow {
    return YES;
}
- (BOOL)windowShouldClose:(id)sender {
    if (_closeFlag)
        *_closeFlag = true;
    return NO;
}

// AppKit routes Cmd-key events through the key window's responder chain
// before falling back to the main menu. The WKWebView in the chain claims
// most shortcuts (Cmd+I, Cmd+O, …) by returning YES from its own
// performKeyEquivalent:, so menu items with those equivalents never fire.
// Give the main menu the first shot here.
- (BOOL)performKeyEquivalent:(NSEvent*)event {
    if ([[NSApp mainMenu] performKeyEquivalent:event]) return YES;
    return [super performKeyEquivalent:event];
}

- (void)broadcastMenuEvent:(NSString*)eventName {
    if (!_editorWebView)
        return;
    NSString* js = [NSString
        stringWithFormat:@"if(window.__traceyBroadcast) window.__traceyBroadcast('{\"event\":\"%@\"}')",
                         eventName];
    [_editorWebView evaluateJavaScript:js completionHandler:nil];
}

- (void)menuImport:(id)sender {
    [self broadcastMenuEvent:@"menu-import"];
}

- (void)menuExport:(id)sender {
    [self broadcastMenuEvent:@"menu-export"];
}

- (void)menuExportGltf:(id)sender {
    [self broadcastMenuEvent:@"menu-export-gltf"];
}

- (void)menuExportGlb:(id)sender {
    [self broadcastMenuEvent:@"menu-export-glb"];
}

- (void)menuExportObj:(id)sender {
    [self broadcastMenuEvent:@"menu-export-obj"];
}

- (void)menuOpenScene:(id)sender {
    [self broadcastMenuEvent:@"menu-open-scene"];
}

- (void)menuSaveScene:(id)sender {
    [self broadcastMenuEvent:@"menu-save-scene"];
}

- (void)menuSaveSceneAs:(id)sender {
    [self broadcastMenuEvent:@"menu-save-scene-as"];
}
@end

// ─── Metal-backed viewport view ─────────────────────────────────────────────
// Hosts the CAMetalLayer that the Vulkan presenter draws into via MoltenVK,
// and forwards mouse / keyboard input into the editor's InputState.
@interface TraceyMetalView : NSView
@property(nonatomic, strong) CAMetalLayer* metalLayer;
@property(nonatomic, assign) tracey_editor::InputState* inputState;
@end

@implementation TraceyMetalView

- (instancetype)initWithFrame:(NSRect)frame {
    self = [super initWithFrame:frame];
    if (self) {
        _metalLayer = [CAMetalLayer layer];
        // Path tracer's resolve shader writes already tonemapped+gamma'd
        // pixels into outputImage, which is what we blit. UNORM (not sRGB)
        // matches the swapchain format.
        _metalLayer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        _metalLayer.framebufferOnly = NO;
        _metalLayer.displaySyncEnabled = YES;
        self.wantsLayer = YES;
        self.layer = _metalLayer;
    }
    return self;
}

// Zero every held movement key. Called when viewport focus changes: if focus
// leaves the native view (e.g. the user clicks into the WebView UI) mid-press,
// the matching keyUp is delivered elsewhere and never reaches us — without
// this the key stays "down" and the camera flies forever. Clearing on BOTH
// resign and become also recovers from a keyUp lost while the app was inactive
// (Cmd-Tab away while holding a key, release, Cmd-Tab back).
- (void)clearMovementKeys {
    if (!_inputState) return;
    _inputState->key_w = false;
    _inputState->key_a = false;
    _inputState->key_s = false;
    _inputState->key_d = false;
    _inputState->key_q = false;
    _inputState->key_e = false;
    _inputState->key_shift = false;
}
- (BOOL)resignFirstResponder {
    [self clearMovementKeys];
    return [super resignFirstResponder];
}
- (BOOL)becomeFirstResponder {
    [self clearMovementKeys];
    return [super becomeFirstResponder];
}

- (BOOL)acceptsFirstResponder {
    return YES;
}
- (BOOL)canBecomeKeyView {
    return YES;
}

// Deliver the FIRST click straight to the viewport even when the editor window
// was inactive (you Cmd-Tabbed away or clicked another app, then clicked back
// into the viewport). Without this, AppKit swallows that first click just to
// activate the window — mouseDown: never fires, focus isn't grabbed, and the
// click appears to "do nothing" until you click a second time. Intermittent
// because it only bites when the window wasn't already key. A 3D viewport should
// always react to a click immediately, so we opt in unconditionally.
- (BOOL)acceptsFirstMouse:(NSEvent*)event {
    (void)event;
    return YES;
}

// ─── Mouse ──
- (void)updateMousePosition:(NSEvent*)event {
    if (!_inputState) return;
    NSPoint loc = [self convertPoint:event.locationInWindow fromView:nil];
    _inputState->mouse_x = loc.x;
    _inputState->mouse_y = self.bounds.size.height - loc.y;
    _inputState->mouse_dx += event.deltaX;
    _inputState->mouse_dy += event.deltaY;
}

- (void)mouseMoved:(NSEvent*)event { [self updateMousePosition:event]; }
- (void)mouseDragged:(NSEvent*)event { [self updateMousePosition:event]; }
- (void)rightMouseDragged:(NSEvent*)event { [self updateMousePosition:event]; }
- (void)otherMouseDragged:(NSEvent*)event { [self updateMousePosition:event]; }

- (void)mouseDown:(NSEvent*)event {
    if (_inputState) _inputState->mouse_left = true;
    // Claim keyboard focus so WASD/QE/Shift land here, not the WKWebView.
    // Space deliberately is NOT consumed by this view (see handleKey) — it
    // belongs to the timeline play/pause shortcut owned by the JS side, so
    // we forward Space presses to the WebView even when the viewport has
    // focus.
    [self.window makeFirstResponder:self];
}
- (void)mouseUp:(NSEvent*)event {
    if (_inputState) _inputState->mouse_left = false;
}
- (void)rightMouseDown:(NSEvent*)event {
    if (_inputState) _inputState->mouse_right = true;
}
- (void)rightMouseUp:(NSEvent*)event {
    if (_inputState) _inputState->mouse_right = false;
}
- (void)otherMouseDown:(NSEvent*)event {
    if (_inputState) _inputState->mouse_middle = true;
}
- (void)otherMouseUp:(NSEvent*)event {
    if (_inputState) _inputState->mouse_middle = false;
}

- (void)scrollWheel:(NSEvent*)event {
    if (!_inputState) return;
    _inputState->scroll_dx += event.scrollingDeltaX;
    _inputState->scroll_dy += event.scrollingDeltaY;
}

// ─── Keyboard ──
- (void)keyDown:(NSEvent*)event {
    // Space is reserved for the timeline (play/pause) and isn't a camera
    // modifier any more. We don't track it on the input_state, and we
    // forward the keypress to the WebView so the App.tsx window listener
    // still fires when the viewport has keyboard focus.
    if (event.keyCode == 49) {
        [self forwardKeyEventToWebView:event];
        return;
    }
    [self handleKey:event pressed:YES];
}
- (void)keyUp:(NSEvent*)event {
    if (event.keyCode == 49) {
        [self forwardKeyEventToWebView:event];
        return;
    }
    [self handleKey:event pressed:NO];
}
- (BOOL)performKeyEquivalent:(NSEvent*)event {
    if (event.keyCode == 49) {  // Space → timeline play/pause (JS owns it)
        [self forwardKeyEventToWebView:event];
        return YES;
    }
    // performKeyEquivalent fires for key-DOWNs regardless of focus and has NO
    // matching key-UP. So ONLY claim edge-triggered one-shots here (framing).
    // Held movement keys (WASD/QE) must flow through keyDown:/keyUp: instead —
    // claiming them here means their key-up is never delivered, leaving the key
    // stuck "down" and the camera flying to infinity. They require viewport
    // focus (a click), which keyDown: already gates on.
    switch (event.keyCode) {
    case 3:    // F / Shift+F — frame selected / all
    case 115:  // Home — frame all
        [self handleKey:event pressed:YES];
        return YES;
    default:
        return NO;  // WASD/QE etc. → handled by keyDown:/keyUp: when focused
    }
}

// Dispatch a synthetic KeyboardEvent on `window` so the App's top-level
// keydown/keyup listeners (timeline play/pause) fire regardless of which
// element actually has DOM focus. We can't simply forward the NSEvent to
// the WKWebView because the WebView only delivers keys to its current
// focused DOM node — not to window-level listeners unless an element is
// focused there. JS-synthesised events bypass that gating.
- (void)forwardKeyEventToWebView:(NSEvent*)event {
    TraceyNSWindow* win = (TraceyNSWindow*)self.window;
    if (![win isKindOfClass:[TraceyNSWindow class]] || !win.editorWebView) return;
    NSString* type = (event.type == NSEventTypeKeyDown) ? @"keydown" : @"keyup";
    // Only Space is forwarded today; keycode 49 maps to " " / "Space".
    NSString* js = [NSString
        stringWithFormat:@"window.dispatchEvent(new KeyboardEvent('%@', {key:' ',code:'Space',keyCode:32,which:32,bubbles:true,cancelable:true}))",
                         type];
    [win.editorWebView evaluateJavaScript:js completionHandler:nil];
}

- (void)handleKey:(NSEvent*)event pressed:(BOOL)pressed {
    if (!_inputState) return;
    // Don't treat Cmd+key (menu shortcuts like Cmd+S) as camera movement — but
    // ONLY skip the key-DOWN. A key-UP must always be honoured: if the user
    // releases a held movement key while Cmd happens to be down, swallowing the
    // up-event would leave the key "stuck" and the camera flying forever.
    if (pressed && (event.modifierFlags & NSEventModifierFlagCommand)) return;
    switch (event.keyCode) {
    case 13: _inputState->key_w = pressed; break;
    case 0:  _inputState->key_a = pressed; break;
    case 1:  _inputState->key_s = pressed; break;
    case 2:  _inputState->key_d = pressed; break;
    case 12: _inputState->key_q = pressed; break;
    case 14: _inputState->key_e = pressed; break;
    // One-shot framing (edge-triggered; the render tick clears the flag):
    //   F        → frame the selection (or the whole scene if nothing's selected)
    //   Shift+F  → frame the whole scene   (Mac-friendly; laptops have no Home key)
    //   Home     → frame the whole scene   (external keyboards)
    case 3:  // F
        if (pressed) {
            if (event.modifierFlags & NSEventModifierFlagShift) _inputState->frame_all = true;
            else _inputState->frame_selected = true;
        }
        break;
    case 115: if (pressed) _inputState->frame_all = true; break;       // Home
    default: break;
    }
}

- (void)flagsChanged:(NSEvent*)event {
    if (!_inputState) return;
    _inputState->key_shift = (event.modifierFlags & NSEventModifierFlagShift) != 0;
    _inputState->key_alt = (event.modifierFlags & NSEventModifierFlagOption) != 0;
}

@end

static NSString* js_escape_str(const std::string& str) {
    NSString* s = [NSString stringWithUTF8String:str.c_str()];
    s = [s stringByReplacingOccurrencesOfString:@"\\" withString:@"\\\\"];
    s = [s stringByReplacingOccurrencesOfString:@"'" withString:@"\\'"];
    s = [s stringByReplacingOccurrencesOfString:@"\n" withString:@"\\n"];
    s = [s stringByReplacingOccurrencesOfString:@"\r" withString:@"\\r"];
    return s;
}

@interface TraceyConsoleLogHandler : NSObject <WKScriptMessageHandler>
@end

@implementation TraceyConsoleLogHandler
- (void)userContentController:(WKUserContentController*)ctl
      didReceiveScriptMessage:(WKScriptMessage*)message {
    NSString* body = [message.body isKindOfClass:[NSString class]]
                         ? message.body
                         : [NSString stringWithFormat:@"%@", message.body];
    std::printf("[JS] %s\n", body.UTF8String);
}
@end

@interface TraceyMessageHandler : NSObject <WKScriptMessageHandler>
@property(nonatomic, assign) tracey_editor::MessageCallback* callback;
@property(nonatomic, assign) WKWebView* webView;
@end

@implementation TraceyMessageHandler
- (void)userContentController:(WKUserContentController*)ctl
      didReceiveScriptMessage:(WKScriptMessage*)message {
    if (!_callback || !(*_callback))
        return;

    NSString* body = nil;
    if ([message.body isKindOfClass:[NSString class]]) {
        body = message.body;
    } else {
        NSData* data = [NSJSONSerialization dataWithJSONObject:message.body options:0 error:nil];
        if (data)
            body = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
    }
    if (!body)
        return;

    std::string request = [body UTF8String];
    std::string bridge_id;
    try {
        auto j = nlohmann::json::parse(request);
        if (j.contains("__bridge_id")) {
            bridge_id = j["__bridge_id"].get<std::string>();
            j.erase("__bridge_id");
            request = j.dump();
        }
    } catch (...) {
    }

    std::string response = (*_callback)(request);

    NSString* escapedResp = js_escape_str(response);
    NSString* escapedId = [NSString stringWithUTF8String:bridge_id.c_str()];
    NSString* js = [NSString
        stringWithFormat:@"if(window.__traceyResponse) window.__traceyResponse('%@','%@')",
                         escapedId, escapedResp];
    [_webView evaluateJavaScript:js completionHandler:nil];
}
@end

@interface TraceyNavigationDelegate : NSObject <WKNavigationDelegate>
@end

@implementation TraceyNavigationDelegate
- (void)webView:(WKWebView*)webView
    decidePolicyForNavigationAction:(WKNavigationAction*)action
                    decisionHandler:(void (^)(WKNavigationActionPolicy))handler {
    handler(WKNavigationActionPolicyAllow);
}

- (void)webView:(WKWebView*)webView didFinishNavigation:(WKNavigation*)navigation {
    std::printf("[WKWebView] Page loaded: %s\n", webView.URL.absoluteString.UTF8String);
}

- (void)webView:(WKWebView*)webView
    didFailNavigation:(WKNavigation*)navigation
            withError:(NSError*)error {
    std::printf("[WKWebView] Navigation failed: %s\n", error.localizedDescription.UTF8String);
}

- (void)webView:(WKWebView*)webView
    didFailProvisionalNavigation:(WKNavigation*)navigation
                       withError:(NSError*)error {
    std::printf("[WKWebView] Provisional navigation failed: %s\n",
                error.localizedDescription.UTF8String);
}
@end

namespace tracey_editor {

struct MacEditorWindow : EditorWindow {
    TraceyNSWindow* window = nil;
    WKWebView* webview = nil;
    TraceyMetalView* metalView = nil;
    TraceyMessageHandler* msg_handler = nil;
    TraceyNavigationDelegate* nav_delegate = nil;

    InputState input_state{};
    MessageCallback message_cb;
    ResizeCallback resize_cb;
    RenderTickCallback render_tick_cb;

    CVDisplayLinkRef display_link = nullptr;

    // Dedicated render-tick thread. The CVDisplayLink fires on a system thread
    // and USED to dispatch render_tick onto the MAIN queue. But render_tick does
    // real work (drain cook results, re-evaluate animation + rebuild the TLAS,
    // present), and on a heavy scene that occupied the main thread long enough to
    // starve the WebView — the whole UI froze during playback while the Metal
    // viewport kept animating. So the display-link callback now just SIGNALS this
    // thread, which runs render_tick OFF the main thread; the main thread is then
    // free to service the WebView + IPC. The viewport present is a MoltenVK/Vulkan
    // present, which is safe to call off the main thread. `tick_pending` coalesces
    // ticks that fire while one is still running into a single pending run, so a
    // renderer slower than the refresh rate never builds a backlog (the role the
    // old `tick_in_flight` atomic played).
    std::thread             tick_thread;
    std::mutex              tick_mutex;
    std::condition_variable tick_cv;
    bool                    tick_pending = false;
    bool                    tick_thread_exit = false;

    uint32_t vp_w = 0;
    uint32_t vp_h = 0;
    uint32_t vp_pixel_w = 0;
    uint32_t vp_pixel_h = 0;
    bool visible = false;
    // Frontend's last explicit wish for the Metal viewport overlay. Tracked
    // separately because set_viewport_rect runs on every scroll/resize and
    // must NOT override an explicit hide (e.g. while the material-graph modal
    // is open above the WebView). Defaults to visible; the first rect report
    // unhides the overlay (it starts hidden until layout is known).
    bool viewport_visible = true;
    bool viewport_rect_known = false;
    bool close_requested = false;

    ~MacEditorWindow() override {
        stop_render_tick();
        if (display_link) {
            CVDisplayLinkRelease(display_link);
            display_link = nullptr;
        }
        if (window) {
            [window close];
            window = nil;
        }
    }

    bool create(uint32_t width, uint32_t height, const char* title) override {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

        NSMenu* menubar = [[NSMenu alloc] init];
        [NSApp setMainMenu:menubar];

        NSMenuItem* appMenuItem = [[NSMenuItem alloc] init];
        [menubar addItem:appMenuItem];
        NSMenu* appMenu = [[NSMenu alloc] init];
        [appMenu addItemWithTitle:@"Quit Tracey Editor"
                           action:@selector(terminate:)
                    keyEquivalent:@"q"];
        [appMenuItem setSubmenu:appMenu];

        NSMenuItem* fileMenuItem = [[NSMenuItem alloc] init];
        [menubar addItem:fileMenuItem];
        NSMenu* fileMenu = [[NSMenu alloc] initWithTitle:@"File"];
        [fileMenu addItemWithTitle:@"Open Scene..."
                            action:@selector(menuOpenScene:)
                     keyEquivalent:@"o"];
        // "Save Scene" — no ellipsis, no dialog when a current path exists.
        // The frontend tracks the last-loaded / last-saved path and falls
        // back to a Save-As prompt only when there's nothing to save to yet.
        [fileMenu addItemWithTitle:@"Save Scene"
                            action:@selector(menuSaveScene:)
                     keyEquivalent:@"s"];
        // Cmd+Shift+S forces a Save As… dialog. NSMenuItem's
        // keyEquivalentModifierMask defaults to Cmd; we add Shift so the
        // capital-S equivalent registers as Cmd+Shift+S (not Cmd+S).
        {
            NSMenuItem* saveAs =
                [fileMenu addItemWithTitle:@"Save Scene As..."
                                    action:@selector(menuSaveSceneAs:)
                             keyEquivalent:@"s"];
            saveAs.keyEquivalentModifierMask =
                NSEventModifierFlagCommand | NSEventModifierFlagShift;
        }
        [fileMenu addItem:[NSMenuItem separatorItem]];
        [fileMenu addItemWithTitle:@"Import..."
                            action:@selector(menuImport:)
                     keyEquivalent:@"i"];
        [fileMenu addItemWithTitle:@"Export Image..."
                            action:@selector(menuExport:)
                     keyEquivalent:@"e"];
        {
            NSMenuItem* exportGeo =
                [fileMenu addItemWithTitle:@"Export Geometry" action:nil keyEquivalent:@""];
            NSMenu* geoMenu = [[NSMenu alloc] initWithTitle:@"Export Geometry"];
            [geoMenu addItemWithTitle:@"glTF (.gltf)..."
                               action:@selector(menuExportGltf:)
                        keyEquivalent:@""];
            [geoMenu addItemWithTitle:@"glTF Binary (.glb)..."
                               action:@selector(menuExportGlb:)
                        keyEquivalent:@""];
            [geoMenu addItemWithTitle:@"Wavefront (.obj)..."
                               action:@selector(menuExportObj:)
                        keyEquivalent:@""];
            [exportGeo setSubmenu:geoMenu];
        }
        [fileMenuItem setSubmenu:fileMenu];

        NSMenuItem* editMenuItem = [[NSMenuItem alloc] init];
        [menubar addItem:editMenuItem];
        NSMenu* editMenu = [[NSMenu alloc] initWithTitle:@"Edit"];
        [editMenu addItemWithTitle:@"Cut" action:@selector(cut:) keyEquivalent:@"x"];
        [editMenu addItemWithTitle:@"Copy" action:@selector(copy:) keyEquivalent:@"c"];
        [editMenu addItemWithTitle:@"Paste" action:@selector(paste:) keyEquivalent:@"v"];
        [editMenu addItemWithTitle:@"Select All" action:@selector(selectAll:) keyEquivalent:@"a"];
        [editMenuItem setSubmenu:editMenu];

        [NSApp finishLaunching];

        // Default the window to the main display's full visible area
        // (excluding menu bar / Dock). The caller's width/height become the
        // minimum the user can resize back down to. Falls back to the
        // requested size when no screen is reported (headless tests).
        NSScreen* screen = [NSScreen mainScreen];
        NSRect frame = screen ? screen.visibleFrame
                              : NSMakeRect(100, 100, width, height);
        NSWindowStyleMask style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                  NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;

        window = [[TraceyNSWindow alloc] initWithContentRect:frame
                                                   styleMask:style
                                                     backing:NSBackingStoreBuffered
                                                       defer:NO];
        window.closeFlag = &close_requested;
        window.delegate = (id<NSWindowDelegate>)window;
        [window setTitle:[NSString stringWithUTF8String:title]];
        [window setMinSize:NSMakeSize(800, 600)];

        NSView* content = [[NSView alloc] initWithFrame:frame];
        [window setContentView:content];

        WKWebViewConfiguration* config = [[WKWebViewConfiguration alloc] init];
        config.preferences.javaScriptEnabled = YES;
        [config.preferences setValue:@YES forKey:@"allowFileAccessFromFileURLs"];
        [config setValue:@YES forKey:@"allowUniversalAccessFromFileURLs"];

        msg_handler = [[TraceyMessageHandler alloc] init];
        msg_handler.callback = &message_cb;
        [config.userContentController addScriptMessageHandler:msg_handler name:@"tracey"];

        TraceyConsoleLogHandler* consoleHandler = [[TraceyConsoleLogHandler alloc] init];
        [config.userContentController addScriptMessageHandler:consoleHandler name:@"consoleLog"];

        NSString* bridgeJS =
            @"window.__traceyPending = {};\n"
             "window.__traceyReqId = 0;\n"
             "window.__traceyBridge = {\n"
             "  send: function(json) {\n"
             "    return new Promise(function(resolve) {\n"
             "      var id = '__req_' + (++window.__traceyReqId);\n"
             "      window.__traceyPending[id] = resolve;\n"
             "      var msg = JSON.parse(json);\n"
             "      msg.__bridge_id = id;\n"
             "      window.webkit.messageHandlers.tracey.postMessage(JSON.stringify(msg));\n"
             "    });\n"
             "  }\n"
             "};\n"
             "window.__traceyResponse = function(bridgeId, json) {\n"
             "  var resolve = window.__traceyPending[bridgeId];\n"
             "  if (resolve) {\n"
             "    delete window.__traceyPending[bridgeId];\n"
             "    resolve(json);\n"
             "  }\n"
             "};\n"
             "(function() {\n"
             "  var origLog = console.log, origWarn = console.warn, origError = console.error;\n"
             "  function fmt(a) {\n"
             "    if (a instanceof Error) {\n"
             "      return a.stack || (a.name + ': ' + a.message);\n"
             "    }\n"
             "    if (a === null || a === undefined) return String(a);\n"
             "    if (typeof a === 'object') {\n"
             "      try { return JSON.stringify(a); } catch(e) { return String(a); }\n"
             "    }\n"
             "    return String(a);\n"
             "  }\n"
             "  function forward(level, args) {\n"
             "    var msg = level + ': ' + Array.from(args).map(fmt).join(' ');\n"
             "    window.webkit.messageHandlers.consoleLog.postMessage(msg);\n"
             "  }\n"
             "  console.log = function() { forward('LOG', arguments); origLog.apply(console, "
             "arguments); };\n"
             "  console.warn = function() { forward('WARN', arguments); origWarn.apply(console, "
             "arguments); };\n"
             "  console.error = function() { forward('ERROR', arguments); origError.apply(console, "
             "arguments); };\n"
             "  window.onerror = function(msg, src, line, col, err) {\n"
             "    forward('UNCAUGHT', [msg, 'at', src + ':' + line + ':' + col]);\n"
             "  };\n"
             "  window.onunhandledrejection = function(e) {\n"
             "    forward('UNHANDLED_REJECTION', [e.reason]);\n"
             "  };\n"
             "})();\n";

        WKUserScript* script =
            [[WKUserScript alloc] initWithSource:bridgeJS
                                   injectionTime:WKUserScriptInjectionTimeAtDocumentStart
                                forMainFrameOnly:YES];
        [config.userContentController addUserScript:script];

        webview = [[TraceyWebView alloc] initWithFrame:content.bounds configuration:config];
        webview.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        // Allow attaching the Safari Web Inspector to the editor UI (Develop menu
        // → this app, or right-click → Inspect Element). Needed to profile the
        // WebView's own main thread — DOM size, scripting vs layout vs paint —
        // when the UI gets sluggish on heavy scenes. macOS 13.3+.
        if (@available(macOS 13.3, *)) {
            webview.inspectable = YES;
        }
        msg_handler.webView = webview;
        window.editorWebView = webview;

        nav_delegate = [[TraceyNavigationDelegate alloc] init];
        webview.navigationDelegate = nav_delegate;

        [content addSubview:webview];

        // Metal viewport overlay — hidden until the frontend reports its rect.
        metalView = [[TraceyMetalView alloc] initWithFrame:NSMakeRect(0, 0, 100, 100)];
        metalView.inputState = &input_state;
        metalView.hidden = YES;
        [content addSubview:metalView positioned:NSWindowAbove relativeTo:webview];

        [window setAcceptsMouseMovedEvents:YES];
        [window center];

        return true;
    }

    void load_ui(const std::string& url_or_path) override {
        NSString* path = [NSString stringWithUTF8String:url_or_path.c_str()];

        if ([path hasPrefix:@"http://"] || [path hasPrefix:@"https://"]) {
            NSURL* url = [NSURL URLWithString:path];
            [webview loadRequest:[NSURLRequest requestWithURL:url]];
        } else {
            NSURL* fileURL = [NSURL fileURLWithPath:path];
            NSURL* dirURL = [fileURL URLByDeletingLastPathComponent];
            [webview loadFileURL:fileURL allowingReadAccessToURL:dirURL];
        }
    }

    void send_to_webview(const std::string& json) override {
        NSString* js = [NSString
            stringWithFormat:@"if(window.__traceyBroadcast) window.__traceyBroadcast('%@')",
                             js_escape_str(json)];

        dispatch_async(dispatch_get_main_queue(),
                       ^{ [webview evaluateJavaScript:js completionHandler:nil]; });
    }

    void set_message_handler(MessageCallback cb) override { message_cb = std::move(cb); }
    void set_resize_callback(ResizeCallback cb) override { resize_cb = std::move(cb); }

    // ── GPU viewport ──
    void* gpu_surface() override { return (__bridge void*)metalView.metalLayer; }
    uint32_t viewport_width() const override { return vp_w; }
    uint32_t viewport_height() const override { return vp_h; }
    uint32_t viewport_pixel_width() const override { return vp_pixel_w; }
    uint32_t viewport_pixel_height() const override { return vp_pixel_h; }

    void set_viewport_rect(int32_t x, int32_t y, uint32_t w, uint32_t h) override {
        // Convert from top-left origin (web) to bottom-left origin (Cocoa)
        NSRect contentBounds = [[window contentView] bounds];
        CGFloat cocoa_y = contentBounds.size.height - y - h;
        [metalView setFrame:NSMakeRect(x, cocoa_y, w, h)];
        viewport_rect_known = true;
        metalView.hidden = !viewport_visible;

        const CGFloat scale = window.backingScaleFactor;
        const uint32_t pw = static_cast<uint32_t>(w * scale);
        const uint32_t ph = static_cast<uint32_t>(h * scale);
        const bool resized = (pw != vp_pixel_w) || (ph != vp_pixel_h);

        vp_w = w;
        vp_h = h;
        vp_pixel_w = pw;
        vp_pixel_h = ph;

        metalView.metalLayer.drawableSize = CGSizeMake(pw, ph);
        input_state.viewport_w = static_cast<float>(w);
        input_state.viewport_h = static_cast<float>(h);

        if (resized && resize_cb) resize_cb(pw, ph);
    }

    void set_viewport_visible(bool vis) override {
        viewport_visible = vis;
        // Only apply once the frontend has reported a rect; otherwise we'd
        // reveal a 100x100 stub at the origin during startup.
        if (viewport_rect_known) metalView.hidden = !vis;
    }
    void set_viewport_accepts_mouse(bool accept) override {
        if (accept) [window makeFirstResponder:metalView];
    }
    InputState& input() override { return input_state; }

    // ── Render tick (CVDisplayLink) ──
    void set_render_tick(RenderTickCallback cb) override { render_tick_cb = std::move(cb); }

    static CVReturn display_link_cb(CVDisplayLinkRef /*link*/, const CVTimeStamp* /*now*/,
                                    const CVTimeStamp* /*outputTime*/, CVOptionFlags /*flagsIn*/,
                                    CVOptionFlags* /*flagsOut*/, void* userInfo) {
        auto* self = static_cast<MacEditorWindow*>(userInfo);
        // Just wake the render-tick thread. If a tick is already running, this
        // collapses into a single pending run (no backlog) — the worker clears
        // tick_pending when it starts a run.
        {
            std::lock_guard<std::mutex> lk(self->tick_mutex);
            self->tick_pending = true;
        }
        self->tick_cv.notify_one();
        return kCVReturnSuccess;
    }

    void start_render_tick() override {
        if (display_link) return;
        // Render-tick worker: blocks until the display link (or a forced wake)
        // signals, then runs render_tick OFF the main thread. See the member
        // declarations for why this is not on the main queue any more.
        tick_thread = std::thread([this] {
            for (;;) {
                {
                    std::unique_lock<std::mutex> lk(tick_mutex);
                    tick_cv.wait(lk, [this] { return tick_pending || tick_thread_exit; });
                    if (tick_thread_exit) return;
                    tick_pending = false;
                }
                if (render_tick_cb) render_tick_cb();
            }
        });
        CVDisplayLinkCreateWithActiveCGDisplays(&display_link);
        CVDisplayLinkSetOutputCallback(display_link, &display_link_cb, this);
        CVDisplayLinkStart(display_link);
    }

    void stop_render_tick() override {
        // Stop new ticks first so the worker won't be re-signalled mid-teardown.
        if (display_link) CVDisplayLinkStop(display_link);
        // Tear down the worker. Idempotent: after the first call tick_thread is
        // joined and not joinable, so the destructor's second call is a no-op.
        {
            std::lock_guard<std::mutex> lk(tick_mutex);
            tick_thread_exit = true;
        }
        tick_cv.notify_one();
        if (tick_thread.joinable()) tick_thread.join();
    }

    void show() override {
        [window makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
        visible = true;
    }

    void hide() override {
        [window orderOut:nil];
        visible = false;
    }

    bool is_visible() const override { return visible; }

    void poll_events() override {
        @autoreleasepool {
            // Block briefly so we yield CPU between display-link ticks. The
            // CVDisplayLink dispatches its render callback to the main queue,
            // which wakes this loop up via its scheduled-source side effect.
            NSDate* until = [NSDate dateWithTimeIntervalSinceNow:1.0 / 240.0];
            NSEvent* event;
            while ((event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                               untilDate:until
                                                  inMode:NSDefaultRunLoopMode
                                                 dequeue:YES])) {
                [NSApp sendEvent:event];
                until = [NSDate distantPast];  // drain the queue, then return
            }
        }
    }

    bool should_close() const override { return close_requested; }

    static NSArray<NSString*>* extensions_to_array(const std::vector<FileFilter>& filters) {
        NSMutableArray<NSString*>* exts = [NSMutableArray array];
        for (const auto& f : filters) {
            for (const auto& ext : f.extensions) {
                [exts addObject:[NSString stringWithUTF8String:ext.c_str()]];
            }
        }
        return exts;
    }

    std::string open_folder_dialog(const char* title) override {
        NSOpenPanel* panel = [NSOpenPanel openPanel];
        panel.canChooseFiles = NO;
        panel.canChooseDirectories = YES;
        panel.allowsMultipleSelection = NO;
        if (title)
            panel.title = [NSString stringWithUTF8String:title];

        if ([panel runModal] == NSModalResponseOK) {
            return [[panel.URL path] UTF8String];
        }
        return {};
    }

    std::string open_file_dialog(const char* title,
                                 const std::vector<FileFilter>& filters) override {
        NSOpenPanel* panel = [NSOpenPanel openPanel];
        panel.canChooseFiles = YES;
        panel.canChooseDirectories = NO;
        panel.allowsMultipleSelection = NO;
        if (title)
            panel.title = [NSString stringWithUTF8String:title];

        NSArray<NSString*>* exts = extensions_to_array(filters);
        if (exts.count > 0) {
            panel.allowedFileTypes = exts;
        }

        if ([panel runModal] == NSModalResponseOK) {
            return [[panel.URL path] UTF8String];
        }
        return {};
    }

    std::string save_file_dialog(const char* title, const char* default_name,
                                 const std::vector<FileFilter>& filters) override {
        NSSavePanel* panel = [NSSavePanel savePanel];
        if (title)
            panel.title = [NSString stringWithUTF8String:title];
        if (default_name)
            panel.nameFieldStringValue = [NSString stringWithUTF8String:default_name];

        NSArray<NSString*>* exts = extensions_to_array(filters);
        if (exts.count > 0) {
            panel.allowedFileTypes = exts;
        }

        if ([panel runModal] == NSModalResponseOK) {
            return [[panel.URL path] UTF8String];
        }
        return {};
    }

    std::string prompt_text(const char* title, const char* message,
                            const char* default_value) override {
        NSAlert* alert = [[NSAlert alloc] init];
        alert.messageText = title ? [NSString stringWithUTF8String:title] : @"";
        if (message) alert.informativeText = [NSString stringWithUTF8String:message];
        [alert addButtonWithTitle:@"OK"];
        [alert addButtonWithTitle:@"Cancel"];

        NSTextField* input = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 260, 24)];
        input.stringValue = default_value ? [NSString stringWithUTF8String:default_value] : @"";
        alert.accessoryView = input;
        [alert.window setInitialFirstResponder:input];

        if ([alert runModal] == NSAlertFirstButtonReturn) {
            return [[input stringValue] UTF8String];
        }
        return {};
    }
};

std::unique_ptr<EditorWindow> create_editor_window() {
    return std::make_unique<MacEditorWindow>();
}

}  // namespace tracey_editor
