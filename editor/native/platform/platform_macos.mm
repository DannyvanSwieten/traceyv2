#include "platform.hpp"

#include <json.hpp>

#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>

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
    TraceyMessageHandler* msg_handler = nil;
    TraceyNavigationDelegate* nav_delegate = nil;

    MessageCallback message_cb;
    ResizeCallback resize_cb;

    bool visible = false;
    bool close_requested = false;

    ~MacEditorWindow() override {
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
            NSEvent* event;
            while ((event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                               untilDate:nil
                                                  inMode:NSDefaultRunLoopMode
                                                 dequeue:YES])) {
                [NSApp sendEvent:event];
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
