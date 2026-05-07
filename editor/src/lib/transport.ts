// Transport abstraction for the native editor.
// The frontend runs inside a WKWebView (macOS) or WebView2 (Windows) host.
// Commands go through window.__traceyBridge.send (a Promise).
// Broadcasts arrive via window.__traceyBroadcast.

export interface Transport {
  send(json: string): Promise<string>;
  onBroadcast(handler: (json: string) => void): void;
  readonly connected: boolean;
}

declare global {
  interface Window {
    __traceyBridge?: {
      send(json: string): Promise<string>;
    };
    __traceyBroadcast?: (json: string) => void;
  }
}

export class WebViewTransport implements Transport {
  private broadcastHandler: ((json: string) => void) | null = null;

  constructor() {
    window.__traceyBroadcast = (json: string) => {
      this.broadcastHandler?.(json);
    };
  }

  get connected(): boolean {
    return !!window.__traceyBridge;
  }

  async send(json: string): Promise<string> {
    if (!window.__traceyBridge) {
      throw new Error('Tracey native bridge not available');
    }
    return window.__traceyBridge.send(json);
  }

  onBroadcast(handler: (json: string) => void): void {
    this.broadcastHandler = handler;
  }
}
