#include "platform.hpp"

#include <json.hpp>

#import <Cocoa/Cocoa.h>
#import <CoreVideo/CoreVideo.h>
#import <QuartzCore/CAMetalLayer.h>
#import <WebKit/WebKit.h>

#include <atomic>
#include <cstdio>

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

- (BOOL)acceptsFirstResponder {
    return YES;
}
- (BOOL)canBecomeKeyView {
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
    // Claim keyboard focus so WASD/QE/Shift/Space land here, not the WKWebView.
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
- (void)keyDown:(NSEvent*)event { [self handleKey:event pressed:YES]; }
- (void)keyUp:(NSEvent*)event { [self handleKey:event pressed:NO]; }
- (BOOL)performKeyEquivalent:(NSEvent*)event {
    [self handleKey:event pressed:(event.type == NSEventTypeKeyDown)];
    return YES;
}

- (void)handleKey:(NSEvent*)event pressed:(BOOL)pressed {
    if (!_inputState) return;
    if (event.modifierFlags & NSEventModifierFlagCommand) return;
    switch (event.keyCode) {
    case 13: _inputState->key_w = pressed; break;
    case 0:  _inputState->key_a = pressed; break;
    case 1:  _inputState->key_s = pressed; break;
    case 2:  _inputState->key_d = pressed; break;
    case 12: _inputState->key_q = pressed; break;
    case 14: _inputState->key_e = pressed; break;
    case 49: _inputState->key_space = pressed; break;
    default: break;
    }
}

- (void)flagsChanged:(NSEvent*)event {
    if (!_inputState) return;
    _inputState->key_shift = (event.modifierFlags & NSEventModifierFlagShift) != 0;
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
    // Set to 1 by the display-link callback when a tick has been queued on the
    // main queue and not yet drained — prevents tick backlog if the renderer
    // is slower than the display refresh rate.
    std::atomic<int> tick_in_flight{0};

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
        [fileMenu addItemWithTitle:@"Import..."
                            action:@selector(menuImport:)
                     keyEquivalent:@"i"];
        [fileMenu addItemWithTitle:@"Export Image..."
                            action:@selector(menuExport:)
                     keyEquivalent:@"e"];
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

        NSRect frame = NSMakeRect(100, 100, width, height);
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
             "  function forward(level, args) {\n"
             "    var msg = level + ': ' + Array.from(args).map(function(a) {\n"
             "      try { return typeof a === 'object' ? JSON.stringify(a) : String(a); }\n"
             "      catch(e) { return String(a); }\n"
             "    }).join(' ');\n"
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

        webview = [[WKWebView alloc] initWithFrame:content.bounds configuration:config];
        webview.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
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
        // Drop frames if a tick is already queued and not yet drained.
        int expected = 0;
        if (!self->tick_in_flight.compare_exchange_strong(expected, 1))
            return kCVReturnSuccess;

        dispatch_async(dispatch_get_main_queue(), ^{
            if (self->render_tick_cb) self->render_tick_cb();
            self->tick_in_flight.store(0);
        });
        return kCVReturnSuccess;
    }

    void start_render_tick() override {
        if (display_link) return;
        CVDisplayLinkCreateWithActiveCGDisplays(&display_link);
        CVDisplayLinkSetOutputCallback(display_link, &display_link_cb, this);
        CVDisplayLinkStart(display_link);
    }

    void stop_render_tick() override {
        if (!display_link) return;
        CVDisplayLinkStop(display_link);
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
};

std::unique_ptr<EditorWindow> create_editor_window() {
    return std::make_unique<MacEditorWindow>();
}

}  // namespace tracey_editor
