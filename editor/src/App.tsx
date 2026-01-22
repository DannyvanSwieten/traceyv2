import { Component } from 'solid-js';
import { Viewport } from './components/viewport/Viewport';
import './App.css';

const App: Component = () => {
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
          <Viewport />
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
