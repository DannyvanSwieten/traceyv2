import { Component, createSignal } from 'solid-js';
import { Viewport, ViewportHandle } from './components/viewport/Viewport';
import { SceneMenu } from './components/scene-menu/SceneMenu';
import './App.css';

const App: Component = () => {
  const [currentScene, setCurrentScene] = createSignal<string | null>(null);
  const [isLoading, setIsLoading] = createSignal(false);
  let viewportRef: ViewportHandle | undefined;

  const handleSceneSelect = async (path: string) => {
    if (isLoading() || path === currentScene()) return;

    setIsLoading(true);
    setCurrentScene(path);

    if (viewportRef) {
      await viewportRef.loadScene(path);
    }

    setIsLoading(false);
  };

  return (
    <div class="app">
      <div class="toolbar">
        <h1>Tracey Editor</h1>
      </div>

      <div class="main-content">
        <div class="left-panel panel">
          <h3>Scenes</h3>
          <SceneMenu
            onSceneSelect={handleSceneSelect}
            currentScene={currentScene}
            isLoading={isLoading}
          />
        </div>

        <div class="viewport-container">
          <Viewport ref={(ref) => (viewportRef = ref)} />
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
