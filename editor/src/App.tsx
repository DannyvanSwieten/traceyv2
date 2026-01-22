import { Component, createSignal, onMount } from 'solid-js';
import { invoke } from '@tauri-apps/api/core';
import './App.css';

const App: Component = () => {
  const [status, setStatus] = createSignal('Initializing...');

  onMount(async () => {
    try {
      // Test connection to backend
      const camera = await invoke('get_camera');
      setStatus('Connected to Tracey backend');
      console.log('Initial camera:', camera);
    } catch (e) {
      setStatus(`Error: ${e}`);
      console.error('Failed to connect to backend:', e);
    }
  });

  return (
    <div class="app">
      <div class="toolbar">
        <h1>Tracey Editor</h1>
      </div>

      <div class="main-content">
        <div class="left-panel panel">
          <h3>Scene Hierarchy</h3>
          <p>Scene tree will go here</p>
        </div>

        <div class="viewport-container">
          <div class="viewport">
            <div class="viewport-placeholder">
              <h2>3D Viewport</h2>
              <p>{status()}</p>
              <p style="font-size: 0.9em; opacity: 0.7;">
                Renderer ready - awaiting integration
              </p>
            </div>
          </div>
        </div>

        <div class="right-panel panel">
          <h3>Properties</h3>
          <p>Properties panel will go here</p>
        </div>
      </div>
    </div>
  );
};

export default App;
