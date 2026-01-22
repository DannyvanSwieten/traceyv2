import { Component, For } from 'solid-js';
import './SceneMenu.css';

interface Scene {
  name: string;
  path: string;
}

interface SceneMenuProps {
  onSceneSelect: (path: string) => void;
  currentScene: () => string | null;
  isLoading: () => boolean;
}

const SCENES: Scene[] = [
  { name: 'Damaged Helmet', path: '/Users/dannyvanswieten/Documents/code/tracey/examples/scenes/DamagedHelmet.glb' },
  { name: 'Avocado', path: '/Users/dannyvanswieten/Documents/code/tracey/examples/scenes/Avocado.glb' },
  { name: 'Duck', path: '/Users/dannyvanswieten/Documents/code/tracey/examples/scenes/Duck.glb' },
  { name: 'Box Textured', path: '/Users/dannyvanswieten/Documents/code/tracey/examples/scenes/BoxTextured.glb' },
  { name: 'Cornell Box', path: '/Users/dannyvanswieten/Documents/code/tracey/examples/scenes/cornell_box.gltf' },
  { name: 'Cornell Box (Closed)', path: '/Users/dannyvanswieten/Documents/code/tracey/examples/scenes/cornell_box_closed.gltf' },
];

export const SceneMenu: Component<SceneMenuProps> = (props) => {
  return (
    <div class="scene-menu">
      <ul class="scene-list">
        <For each={SCENES}>
          {(scene) => (
            <li
              class="scene-item"
              classList={{
                'scene-item--active': props.currentScene() === scene.path,
                'scene-item--disabled': props.isLoading(),
              }}
              onClick={() => {
                if (!props.isLoading()) {
                  props.onSceneSelect(scene.path);
                }
              }}
            >
              <span class="scene-icon">📦</span>
              <span class="scene-name">{scene.name}</span>
            </li>
          )}
        </For>
      </ul>
    </div>
  );
};
